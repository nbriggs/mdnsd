/*
 * Copyright (c) 2010 Christiano F. Haesbaert <haesbaert@haesbaert.org>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <sys/time.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/utsname.h>

#include <netinet/in.h>
#include <arpa/inet.h>

#include <err.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>

#include "mdnsd.h"
#include "log.h"

int		 cache_insert(struct rr *);
int		 cache_delete(struct rr *);
void		 cache_schedrev(struct rr *);
void		 cache_rev(int, short, void *);
struct rrt_node *cache_lookup_node(struct rrset *);

int 		   question_cmp(struct question *, struct question *);
struct question    *question_lookup(struct rrset *);

void		 rrt_dump(struct rrt_tree *);
int		 rrt_cmp(struct rrt_node *, struct rrt_node *);
struct rr	*rrt_lookup(struct rrt_tree *, struct rrset *);
struct rrt_node	*rrt_lookup_node(struct rrt_tree *, struct rrset *);

RB_GENERATE(rrt_tree,  rrt_node, entry, rrt_cmp);
RB_HEAD(question_tree, question);
RB_PROTOTYPE(question_tree, question, qst_entry, question_cmp);
RB_GENERATE(question_tree, question, qst_entry, question_cmp);

extern struct mdnsd_conf	*conf;
struct question_tree		 question_tree;
struct rrt_tree			 cache_tree;

/*
 * Publishing
 */

void
publish_init(void)
{
	struct iface	*iface;
	struct rr	*rr;
	char		 revaddr[MAXHOSTNAMELEN];

	/* insert default records in all our interfaces */
	LIST_FOREACH(iface, &conf->iface_list, entry) {
		/* myname */
		if ((rr = calloc(1, sizeof(*rr))) == NULL)
			fatal("calloc");
		rr_set(rr, conf->myname, T_A, C_IN, TTL_HNAME, 1,
		    &iface->addr, sizeof(iface->addr));
		if (publish_insert(iface, rr) == -1)
			log_debug("publish_init: can't insert rr");

		/* publish reverse address */
		if ((rr = calloc(1, sizeof(*rr))) == NULL)
			fatal("calloc");
		reversstr(revaddr, &iface->addr);
		rr_set(rr, revaddr, T_PTR, C_IN, TTL_HNAME, 1,
		    conf->myname, sizeof(conf->myname));
		if (publish_insert(iface, rr) == -1)
			log_debug("publish_init: can't insert rr");

		/* publish hinfo */
		if ((rr = calloc(1, sizeof(*rr))) == NULL)
			fatal("calloc");
		rr_set(rr, conf->myname, T_HINFO, C_IN, TTL_HNAME, 1,
		    &conf->hi, sizeof(conf->hi));
		if (publish_insert(iface, rr) == -1)
			log_debug("publish_init: can't insert rr");
	}
}

void
publish_allrr(struct iface *iface)
{
	struct question		*qst;
	struct rr		*rr, *rrcopy;
	struct publish		*pub;
	struct rrt_node		*n;
	struct timeval		 tv;

	/* start a publish thingy */
	if ((pub = calloc(1, sizeof(*pub))) == NULL)
		fatal("calloc");
	pub->state = PUB_INITIAL;
	pkt_init(&pub->pkt);
	if ((qst = calloc(1, sizeof(*qst))) == NULL)
		fatal("calloc");
	strlcpy(qst->rrs.dname, conf->myname, sizeof(qst->rrs.dname));
	qst->rrs.type  = T_ANY;
	qst->rrs.class = C_IN;
	pub->pkt.h.qr = MDNS_QUERY;
	pkt_add_question(&pub->pkt, qst);

	RB_FOREACH(n, rrt_tree, &iface->rrt) {
		/* now go through all our rr and add to the same packet */
		LIST_FOREACH(rr, &n->hrr, centry) {
			if ((rrcopy = calloc(1, sizeof(struct rr))) == NULL)
				fatal("calloc");
			memcpy(rrcopy, rr, sizeof(struct rr));
			pkt_add_nsrr(&pub->pkt, rrcopy);
		}
	}

	timerclear(&tv);
	tv.tv_usec = RANDOM_PROBETIME;
	evtimer_set(&pub->timer, publish_fsm, pub);
	evtimer_add(&pub->timer, &tv);
}

int
publish_delete(struct iface *iface, struct rr *rr)
{
	struct rr	*rraux, *next;
	struct rrt_node	*s;
	int		 n = 0;

	log_debug("publish_delete: type: %s name: %s",
	    rr_type_name(rr->rrs.type), rr->rrs.dname);
	s = rrt_lookup_node(&iface->rrt, &rr->rrs);
	if (s == NULL)
		return (0);

	for (rraux = LIST_FIRST(&s->hrr); rraux != NULL; rraux = next) {
		next = LIST_NEXT(rraux, centry);
		if (RR_UNIQ(rr) || /* XXX: Revise this */
		    (rr_rdata_cmp(rr, rraux) == 0)) {
			LIST_REMOVE(rraux, centry);
			free(rraux);
			n++;
		}
	}

	if (LIST_EMPTY(&s->hrr)) {
		RB_REMOVE(rrt_tree, &iface->rrt, s);
		free(s);
	}

	return (n);
}

int
publish_insert(struct iface *iface, struct rr *rr)
{
	struct rrt_node *n;
	struct rr	*rraux;

	log_debug("publish_insert: type: %s name: %s",
	    rr_type_name(rr->rrs.type), rr->rrs.dname);

	n = rrt_lookup_node(&iface->rrt, &rr->rrs);
	if (n == NULL) {
		if ((n = calloc(1, sizeof(*n))) == NULL)
			fatal("calloc");
		n->rrs = rr->rrs;
		LIST_INIT(&n->hrr);
		LIST_INSERT_HEAD(&n->hrr, rr, centry);
		if (RB_INSERT(rrt_tree, &iface->rrt, n) != NULL)
			fatal("rrt_insert: RB_INSERT");

		return (0);
	}

	/* if an unique record, clean all previous and substitute */
	if (RR_UNIQ(rr)) {
		while ((rraux = LIST_FIRST(&n->hrr)) != NULL) {
			LIST_REMOVE(rraux, centry);
			free(rraux);
		}
		LIST_INSERT_HEAD(&n->hrr, rr, centry);

		return (0);
	}

	/* not unique, just add */
	LIST_INSERT_HEAD(&n->hrr, rr, centry);

	return (0);
}

/* XXX: if query type is ANY, won't match. */
struct rr *
publish_lookupall(struct rrset *rrs)
{
	struct iface	*iface;
	struct rr	*rr;

	LIST_FOREACH(iface, &conf->iface_list, entry) {
		rr = rrt_lookup(&iface->rrt, rrs);
		if (rr != NULL)
			return (rr);
	}

	return (NULL);
}

void
publish_fsm(int unused, short event, void *v_pub)
{
	struct publish	*pub = v_pub;
	struct timeval	 tv;
	struct rr	*rr;
	struct question	*qst;
	static u_long	 pubid;
	
	timerclear(&tv);
	switch (pub->state) {
	case PUB_INITIAL:
		pub->state = PUB_PROBE;
		pub->id = ++pubid;
		/* FALLTHROUGH */
	case PUB_PROBE:
		pub->pkt.h.qr = MDNS_QUERY;
		if (pkt_send_allif(&pub->pkt) == -1)
			log_debug("can't send packet to all interfaces");
		pub->sent++;
		/* Send only the first 2 question as QU */
		if (pub->sent == 2)
			LIST_FOREACH(qst, &pub->pkt.qlist, entry)
				qst->src.s_addr = 0;
		/* enough probing, start announcing */
		else if (pub->sent == 3) { 
			/* cool, so now that we're done, remove it from
			 * probing list, now the record is ours. */
			pub->state    = PUB_ANNOUNCE;
			pub->sent     = 0;
			pub->pkt.h.qr = MDNS_RESPONSE;
			/* remove questions */
			while ((qst = (LIST_FIRST(&pub->pkt.qlist))) != NULL) {
				LIST_REMOVE(qst, entry);
				pub->pkt.h.qdcount--;
				free(qst);
			}
			/* move all ns records to answer records */
			while ((rr = (LIST_FIRST(&pub->pkt.nslist))) != NULL) {
				LIST_REMOVE(rr, pentry);
				pub->pkt.h.nscount--;
				if (pkt_add_anrr(&pub->pkt, rr) == -1)
					log_debug("publish_fsm: "
					    "pkt_add_anrr failed");
			}
			publish_fsm(unused, event, pub);
			return;
		}
		tv.tv_usec = INTERVAL_PROBETIME;
		evtimer_add(&pub->timer, &tv);
		break;
	case PUB_ANNOUNCE:
		if (pkt_send_allif(&pub->pkt) == -1)
			log_debug("can't send packet to all interfaces");
		pub->sent++;
		if (pub->sent < 3) {
			tv.tv_sec = pub->sent; /* increse delay linearly */
			evtimer_add(&pub->timer, &tv);
			return;
		}

		/* sent announcement three times, finish */
		pub->state = PUB_DONE;
		publish_fsm(unused, event, pub);
		break;
	case PUB_DONE:
		while ((rr = LIST_FIRST(&pub->pkt.anlist)) != NULL) {
			LIST_REMOVE(rr, pentry);
			pub->pkt.h.ancount--;
			free(rr);
		}
		while ((rr = LIST_FIRST(&pub->pkt.nslist)) != NULL) {
			LIST_REMOVE(rr, pentry);
			pub->pkt.h.nscount--;
			free(rr);
		}
		while ((rr = LIST_FIRST(&pub->pkt.arlist)) != NULL) {
			LIST_REMOVE(rr, pentry);
			pub->pkt.h.arcount--;
			free(rr);
		}
		while ((qst = LIST_FIRST(&pub->pkt.qlist)) != NULL) {
			LIST_REMOVE(qst, entry);
			pub->pkt.h.qdcount--;
			free(qst);
		}
		free(pub);
		break;
	default:
		fatalx("Unknown publish state, report this");
		break;
	}
}

/*
 * RR cache
 */

void
cache_init(void)
{
#ifdef DUMMY_ENTRIES
	char **nptr;
	struct rr *rr;
	char *tnames[] = {
		"teste1.local",
		"teste2.local",
		"teste3.local",
		"teste4.local",
		"teste5.local",
		"teste6.local",
		"teste7.local",
		"teste8.local",
		"teste9.local",
		"teste10.local",
		"teste11.local",
		"teste12.local",
		"teste13.local",
		"teste14.local",
		"teste15.local",
		"teste16.local",
		"teste17.local",
		"teste18.local",
		"teste19.local",
		"teste20.local",
		"teste21.local",
		"teste22.local",
		"teste23.local",
		"teste24.local",
		"teste25.local",
		"teste26.local",
		"teste27.local",
		"teste28.local",
		"teste29.local",
		"teste30.local",
		"teste31.local",
		"teste32.local",
		"teste33.local",
		"teste34.local",
		"teste35.local",
		"teste36.local",
		"teste37.local",
		"teste38.local",
		"teste39.local",
		"teste40.local",
		"teste41.local",
		"teste42.local",
		"teste43.local",
		"teste44.local",
		"teste45.local",
		"teste46.local",
		"teste47.local",
		"teste48.local",
		"teste49.local",
		"teste50.local",
		0
	};
#endif
	RB_INIT(&cache_tree);
#ifdef DUMMY_ENTRIES
	for (nptr = tnames; *nptr != NULL; nptr++) {
		if ((rr = calloc(1, sizeof(*rr))) == NULL)
			err(1, "calloc");
		strlcpy(rr->dname, "_http._tcp.local", sizeof(rr->dname));
		rr->type = T_PTR;
		rr->class = C_IN;
		rr->ttl = 60;
		strlcpy(rr->rdata.PTR, *nptr, sizeof(rr->rdata.PTR));
		rr->rdlen = strlen(*nptr);
		evtimer_set(&rr->rev_timer, cache_rev, rr);
		cache_insert(rr);

	}

#endif

}

int
cache_process(struct rr *rr)
{
	evtimer_set(&rr->rev_timer, cache_rev, rr);
	if (clock_gettime(CLOCK_MONOTONIC, &rr->age) == -1)
		fatal("clock_gettime");
	/*
	 * If ttl is 0 this is a Goodbye RR. cache_delete() will look for all
	 * corresponding RR in our cache and remove/free them. This rr isn't in
	 * cache, therefore cache_delete() won't free it, this is the only
	 * special case when we call cache_delete() on a rr that isn't in *
	 * cache.
	 */
	
	/* TODO: schedule it for 1 second */
	if (rr->ttl == 0) {
		cache_delete(rr);
		free(rr);
		
		return (0);
	}
	
	if (cache_insert(rr) == -1)
		return (-1);

	return (0);
}

struct rr *
cache_lookup(struct rrset *rrs)
{
	return (rrt_lookup(&cache_tree, rrs));
}

struct rrt_node *
cache_lookup_node(struct rrset *rrs)
{
	return (rrt_lookup_node(&cache_tree, rrs));
}

int
cache_insert(struct rr *rr)
{
	struct rrt_node *n;
	struct rr	*rraux;

/* 	log_debug("cache_insert: type: %s name: %s", rr_type_name(rr->type), */
/* 	    rr->dname); */

	n = cache_lookup_node(&rr->rrs);
	if (n == NULL) {
		if ((n = calloc(1, sizeof(*n))) == NULL)
			fatal("calloc");
		
		n->rrs = rr->rrs;
		LIST_INIT(&n->hrr);
		LIST_INSERT_HEAD(&n->hrr, rr, centry);
		if (RB_INSERT(rrt_tree, &cache_tree, n) != NULL)
			fatal("rrt_insert: RB_INSERT");
		cache_schedrev(rr);
		/* query_notify(rr, 1); */
		rr_notify_in(rr);

		return (0);
	}

	/* if an unique record, clean all previous and substitute */
	if (RR_UNIQ(rr)) {
		while ((rraux = LIST_FIRST(&n->hrr)) != NULL) {
			LIST_REMOVE(rraux, centry);
			if (evtimer_pending(&rraux->rev_timer, NULL))
				evtimer_del(&rraux->rev_timer);
			free(rraux);
		}
		LIST_INSERT_HEAD(&n->hrr, rr, centry);
		cache_schedrev(rr);
/* 		query_notify(rr, 1); */
		rr_notify_in(rr);

		return (0);
	}

	/* rr is not unique, see if this is a cache refresh */
	LIST_FOREACH(rraux, &n->hrr, centry) {
		if (rr_rdata_cmp(rr, rraux) == 0) {
			rraux->ttl = rr->ttl;
			rraux->revision = 0;
			cache_schedrev(rraux);
			free(rr);

			return (0);
		}
	}

	/* not a refresh, so add */
	LIST_INSERT_HEAD(&n->hrr, rr, centry);
	rr_notify_in(rr);
	cache_schedrev(rr);
	/* XXX: should we cache_schedrev ? */

	return (0);
}

int
cache_delete(struct rr *rr)
{
	struct rr	*rraux, *next;
	struct rrt_node	*s;
	int		 n = 0;

	log_debug("cache_delete: type: %s name: %s", rr_type_name(rr->rrs.type),
	    rr->rrs.dname);
/* 	query_notify(rr, 0); */
/* 	rr_notifiy_out(rr); */
	s = cache_lookup_node(&rr->rrs);
	if (s == NULL)
		return (0);

	for (rraux = LIST_FIRST(&s->hrr); rraux != NULL; rraux = next) {
		next = LIST_NEXT(rraux, centry);
		if (RR_UNIQ(rr) ||
		    (rr_rdata_cmp(rr, rraux) == 0)) {
			LIST_REMOVE(rraux, centry);
			if (evtimer_pending(&rraux->rev_timer, NULL))
				evtimer_del(&rraux->rev_timer);
			free(rraux);
			n++;
		}
	}

	if (LIST_EMPTY(&s->hrr)) {
		RB_REMOVE(rrt_tree, &cache_tree, s);
		free(s);
	}

	return (n);
}

void
cache_schedrev(struct rr *rr)
{
	struct timeval tv;
	u_int32_t var;

	timerclear(&tv);

	switch (rr->revision) {
	case 0:
		/* Expire at 80%-82% of ttl */
		var = 80 + arc4random_uniform(3);
		tv.tv_sec = ((10 * rr->ttl) * var) / 1000;
		break;
	case 1:
		/* Expire at 90%-92% of ttl */
		var = 90 + arc4random_uniform(3);
		tv.tv_sec  = ((10 * rr->ttl) * var) / 1000;
		tv.tv_sec -= ((10 * rr->ttl) * 80)  / 1000;
		break;
	case 2:
		/* Expire at 95%-97% of ttl */
		var = 95 + arc4random_uniform(3);
		tv.tv_sec  = ((10 * rr->ttl) * var) / 1000;
		tv.tv_sec -= ((10 * rr->ttl) * 90)  / 1000;
		break;
	case 3:	/* expired, delete from cache in 1 sec */
		tv.tv_sec = 1;
		break;
	}
/* 	log_debug("cache_schedrev: schedule rr type: %s, name: %s (%d)", */
/* 	    rr_type_name(rr->type), rr->dname, tv.tv_sec); */

	rr->revision++;

	if (evtimer_pending(&rr->rev_timer, NULL))
		evtimer_del(&rr->rev_timer);
	if (evtimer_add(&rr->rev_timer, &tv) == -1)
		fatal("rrt_sched_rev");
}

void
cache_rev(int unused, short event, void *v_rr)
{
	struct rr	*rr = v_rr;
	struct question	*qst;
	struct pkt	 pkt;

/* 	log_debug("cache_rev: timeout rr type: %s, name: %s (%u)", */
/* 	    rr_type_name(rr->type), rr->dname, rr->ttl); */

	/* If we have an active question, try to renew the answer */
	if ((qst = question_lookup(&rr->rrs)) != NULL) {
		pkt_init(&pkt);
		pkt.h.qr = MDNS_QUERY;
		pkt_add_question(&pkt, qst);
		if (pkt_send_allif(&pkt) == -1)
			log_warnx("can't send packet to all interfaces");
	}

	if (rr->revision <= 3)
		cache_schedrev(rr);
	else
		cache_delete(rr);
}

/*
 * RR tree
 */

void
rrt_dump(struct rrt_tree *rrt)
{
	struct rr	*rr;
	struct rrt_node *n;

	log_debug("rrt_dump");
	RB_FOREACH(n, rrt_tree, rrt) {
		rr = LIST_FIRST(&n->hrr);
		LIST_FOREACH(rr, &n->hrr, centry)
		    log_debug_rr(rr);
	}
}

struct rr *
rrt_lookup(struct rrt_tree *rrt, struct rrset *rrs)
{
	struct rrt_node *tmp;
	
	tmp = rrt_lookup_node(rrt, rrs);
	if (tmp != NULL)
		return (LIST_FIRST(&tmp->hrr));
	
	return (NULL);
}

struct rrt_node *
rrt_lookup_node(struct rrt_tree *rrt, struct rrset *rrs)
{
	struct rrt_node s;
	
	bzero(&s, sizeof(s));
	s.rrs = *rrs;

	return (RB_FIND(rrt_tree, rrt, &s));
}

int
rrt_cmp(struct rrt_node *a, struct rrt_node *b)
{
	return (rrset_cmp(&a->rrs, &b->rrs));
}

int
rrset_cmp(struct rrset *a, struct rrset *b)
{
	if (a->class < b->class)
		return (-1);
	if (a->class > b->class)
		return (1);
	if (a->type < b->type)
		return (-1);
	if (a->type > b->type)
		return (1);

	return (strcmp(a->dname, b->dname));
}

/*
 * Querier
 */

void
query_init(void)
{
	RB_INIT(&question_tree);
}

struct question *
question_lookup(struct rrset *rrs)
{
	struct question qst;
	
	bzero(&qst, sizeof(qst));
	qst.rrs = *rrs;
	
	return (RB_FIND(question_tree, &question_tree, &qst));
}

struct question *
question_add(struct rrset *rrs)
{
	struct question *qst;

	qst = question_lookup(rrs);
	if (qst != NULL) {
		qst->active++;
		log_debug("existing question for %s (%s) active = %d",
		    rrs->dname, rr_type_name(rrs->type), qst->active);
		return (qst);
	}
	if ((qst = calloc(1, sizeof(*qst))) == NULL)
		fatal("calloc");
	qst->active++;
	qst->rrs = *rrs;
	if (RB_INSERT(question_tree, &question_tree, qst) != NULL)
		fatal("question_add: RB_INSERT");
	
	return (qst);
}

/* struct query * */
/* query_place(enum query_style s, struct rrset *rrs) */
/* { */
/* 	struct query		*q; */
/* 	struct query_node	*qn; */
/* 	struct timeval		 tv; */

/* 	q = query_lookup(rrs); */
/* 	/\* existing query, increase active *\/ */
/* 	if (q != NULL) { */
/* 		if (s != q->style) { */
/* 			log_warnx("trying to change a query style"); */
/* 			return (NULL); */
/* 		} */
/* 		q->active++; */
/* 		log_debug("existing query active = %d", q->active); */
/* 		return (q); */
/* 	} */
/* 	/\* no query, make a new one *\/ */
/* 	log_debug("making new query"); */
/* 	if ((qn = calloc(1, sizeof(*qn))) == NULL) */
/* 		fatal("calloc"); */
/* 	q = &qn->q; */
/* 	q->qst.rrs = *rrs; */
/* 	q->style = s; */
/* 	q->active++; */
/* 	if (RB_INSERT(query_tree, &query_tree, qn) != NULL) */
/* 		fatal("query_place: RB_INSERT"); */
/* 	/\* start the sending machine *\/ */
/* 	timerclear(&tv); */
/* 	tv.tv_usec = FIRST_QUERYTIME; */
/* 	evtimer_set(&q->timer, query_fsm, q); */
/* 	evtimer_add(&q->timer, &tv); */
/* 	return (q); */
/* } */

void
question_remove(struct rrset *rrs)
{
	struct question *qst;

	qst = question_lookup(rrs);
	if (qst == NULL) {
		log_warnx("trying to remove non existant question");
		return;
	}
	if (--qst->active == 0) {
		RB_REMOVE(question_tree, &question_tree, qst);
		free(qst);
	}
}

/* int */
/* query_notify(struct rr *rr, int in) */
/* { */
/* 	struct ctl_conn *c; */
/* 	struct query	*q; */
/* 	int		 tosee; */
/* 	int		 msgtype; */

/* 	q = query_lookup(&rr->rrs); */
/* 	if (q == NULL) */
/* 		return (0); */
/* 	/\* try to answer the controllers *\/ */
/* 	tosee = q->active; */
/* 	TAILQ_FOREACH(c, &ctl_conns, entry) { */
/* 		if (!tosee) */
/* 			break; */
/* 		if (!control_hasq(c, q)) */
/* 			continue; */
/* 		/\* sanity check *\/ */
/* 		if (!ANSWERS(&q->qst, rr)) { */
/* 			log_warnx("Bogus pointer, report me"); */
/* 			return (0); */
/* 		} */
/* 		/\* notify controller *\/ */
/* 		switch (q->style) { */
/* 		case QUERY_LOOKUP: */
/* 			msgtype = IMSG_CTL_LOOKUP; */
/* 			break; */
/* 		case QUERY_BROWSE: */
/* 			msgtype = in ? IMSG_CTL_BROWSE_ADD */
/* 			    : IMSG_CTL_BROWSE_DEL; */
/* 			break; */
/* 		default: */
/* 			log_warnx("Unknown query style"); */
/* 			return (-1); */
/* 		} */
/* 		if (query_answerctl(c, rr, msgtype) == -1) */
/* 			log_warnx("Query_answerctl error"); */
/* 	} */

/* 	/\* number of notified controllers *\/ */
/* 	return (q->active - tosee); */
/* } */

void
query_remove(struct query *q)
{
	struct rr	*rr;

	LIST_REMOVE(q, entry);
	while ((rr = (LIST_FIRST(&q->rrlist))) != NULL) {
		question_remove(&rr->rrs);
		LIST_REMOVE(rr, qentry);
		free(rr);
	}
	if (evtimer_pending(&q->timer, NULL))
		evtimer_del(&q->timer);
	free(q);
}

/* int */
/* rr_notify_out(struct rr *rr) */
/* { */
/* 	struct ctl_conn *c; */
/* 	struct query	*q; */
/* 	struct question *qst; */
/* 	struct rr	*rraux; */
/* 	int		 msgtype; */

/* 	if ((qst = question_lookup(&rr->rrs)) == NULL) */
/* 		return (0); */
	
/* 	TAILQ_FOREACH(c, &ctl_conns, entry) { */
/* 		LIST_FOREACH(q, &c->qlist, entry) { */
/* 			if (q->style == QUERY_LOOKUP) */
/* 				continue; */
/* 			LIST_FOREACH(rraux, &q->rrlist, qentry) { */
/* 				/\* */
/* 				 * Check if controller is interested */
/* 				 * only if question wasn't answered. */
/* 				 *\/ */
/* 				if (rraux->answered || */
/* 				    rrset_cmp(&rr->rrs, &rraux->rrs) != 0) */
/* 					continue; */
/* 				/\* */
/* 				 * Notify controller with full RR. */
/* 				 *\/ */
/* 				if (q->style != QUERY_BROWSE) { */
/* 					log_warnx("Unexpected query style %d", */
/* 					    q->style); */
/* 					return (-1); */
/* 				} */
					
/* 				switch (q->style) { */
/* 				case QUERY_BROWSE: */
/* 					msgtype = IMSG_CTL_BROWSE_ADD; */
/* 					break; */
/* 				default: */
/* 					log_warnx("Unknown query style"); */
/* 					return (-1); */
/* 				} */
/* 				if (control_send_rr(c, rr, msgtype) == -1) */
/* 					log_warnx("control_send_rr error"); */
/* 				rraux->answered = 1; */
/* 			} */
/* 		} */
/* 	} */

/* 	return (0); */
/* } */

int
rr_notify_in(struct rr *rr)
{
	struct ctl_conn *c;
	struct query	*q, *nextq;
	struct question *qst;
	struct rr	*rraux;
	int		 msgtype;

	if ((qst = question_lookup(&rr->rrs)) == NULL)
		return (0);
	
	TAILQ_FOREACH(c, &ctl_conns, entry) {
		for (q = LIST_FIRST(&c->qlist); q != NULL; q = nextq) {
			/* We may delete queries... */
			nextq = LIST_NEXT(q, entry);
			LIST_FOREACH(rraux, &q->rrlist, qentry) {
				/*
				 * Check if controller is interested
				 * only if question wasn't answered.
				 */
				if (rraux->answered ||
				    rrset_cmp(&rr->rrs, &rraux->rrs) != 0)
					continue;
				/*
				 * Notify controller with full RR.
				 */
				switch (q->style) {
				case QUERY_LOOKUP:
					msgtype = IMSG_CTL_LOOKUP;
					break;
				case QUERY_BROWSE:
					msgtype = IMSG_CTL_BROWSE_ADD;
					break;
				default:
					log_warnx("Unknown query style");
					return (-1);
				}
				if (control_send_rr(c, rr, msgtype) == -1)
					log_warnx("control_send_rr error");
				rraux->answered = 1;
				if (q->style == QUERY_LOOKUP) {
					query_remove(q);
					break;
				}
			}
		}
	}

	return (0);
}
/* RR in/out, 1 = in, 0 = out */
/* TODO: Revise this, is all wrong */
/* int */
/* query_notify(struct rr *rr, int in) */
/* { */
/* 	struct ctl_conn *c; */
/* 	struct query	*q; */
/* 	struct question *qst; */
/* 	struct rr	*rraux; */
/* 	int		 msgtype; */

/* 	if ((qst = question_lookup(&rr->rrs)) == NULL) */
/* 		return (0); */
	
/* 	TAILQ_FOREACH(c, &ctl_conns, entry) { */
/* 		/\* */
/* 		 * Check if controller is interested */
/* 		 * only if question wasn't answered. */
/* 		 *\/ */
/* 		LIST_FOREACH(q, &c->qlist, entry) { */
/* 			/\* Lookup only wants in events *\/ */
/* 			if (q->style == QUERY_LOOKUP && in == 0) */
/* 				continue; */
/* 			LIST_FOREACH(rraux, &q->rrlist, qentry) { */
/* 				if (rraux->answered || */
/* 				    rrset_cmp(&rr->rrs, &rraux->rrs) != 0) */
/* 					continue; */
/* 				/\* */
/* 				 * Notify controller with full RR. */
/* 				 *\/ */
/* 				switch (q->style) { */
/* 				case QUERY_LOOKUP: */
/* 					msgtype = IMSG_CTL_LOOKUP; */
/* 					break; */
/* 				case QUERY_BROWSE: */
/* 					msgtype = in ? IMSG_CTL_BROWSE_ADD */
/* 					    : IMSG_CTL_BROWSE_DEL; */
/* 					break; */
/* 				default: */
/* 					log_warnx("Unknown query style"); */
/* 					return (-1); */
/* 				} */
/* 				if (control_send_rr(c, rr, msgtype) == -1) */
/* 					log_warnx("control_send_rr error"); */
/* 				rraux->answered = 1; */
/* 			} */
/* 		} */
/* 	} */

/* 	return (0); */
/* } */

void
query_fsm(int unused, short event, void *v_query)
{
	struct pkt	 pkt;
	struct query	*q;
	struct question	*qst;
	struct rr	*rr, *rraux;
	long		 tosleep;
	struct timespec	 tnow, tdiff;
	struct timeval	 tv;

	q = v_query;
	pkt_init(&pkt);
	pkt.h.qr = MDNS_QUERY;
	
	/* This will send at seconds 0, 1, 2, 4, 8, 16... */
	tosleep = (2 << q->count) - (1 << q->count);
	if (tosleep > MAX_QUERYTIME)
		tosleep = MAX_QUERYTIME;
	timerclear(&tv);
	tv.tv_sec = tosleep;

	if (clock_gettime(CLOCK_MONOTONIC, &tnow) == -1)
		fatal("clock_gettime");
	
	LIST_FOREACH(rr, &q->rrlist, qentry) {
		if ((qst = question_lookup(&rr->rrs)) == NULL) {
			log_warnx("Can't find question in query_fsm");
			/* XXX: we leak memory */
			return;
		}
		/*
		 * If we're in our third call we're still alive,
		 * consider a failure.
		 */
		if (q->style == QUERY_LOOKUP && q->count == 2) {
			control_send_rr(q->ctl, rr, IMSG_CTL_LOOKUP_FAILURE);
			query_remove(q);
			return;
		}

		timespecsub(&tnow, &qst->ts, &tdiff);
		/* Only 1 time a second per question  */
		if (qst->sent > 0 && tdiff.tv_sec < 1) {
			log_debug("query for %s supressed, just sent",
			    rrs_str(&rr->rrs));
			continue;
		}
		
		pkt_add_question(&pkt, qst);
		qst->sent++;
		qst->ts = tnow;
		if (q->style == QUERY_BROWSE) {
			/* Known Answer Supression */
			for (rraux = cache_lookup(&qst->rrs);
			     rraux != NULL;
			     rraux = LIST_NEXT(rraux, centry)) {
				/* Don't include rr if it's too old */
				if (rr_ttl_left(rraux) < rraux->ttl / 2)
					continue;
				if (pkt_add_anrr(&pkt, rraux) == -1)
					log_warnx("KNA error pkt_add_anrr: %s",
					    rraux->rrs.dname);
			}
		}
	}

	if (pkt.h.qdcount > 0)
		if (pkt_send_allif(&pkt) == -1)
			log_warnx("can't send packet to all interfaces");
	q->count++;
	evtimer_add(&q->timer, &tv);
}

/* void */
/* query_fsm(int unused, short event, void *v_query) */
/* { */
/* 	struct pkt	 pkt; */
/* 	struct timeval	 tv; */
/* 	struct query	*q; */
/* 	struct rr	*rr; */
/* 	long		 tosleep; */

/* 	q = v_query; */
/* 	pkt_init(&pkt); */
/* 	pkt.h.qr = MDNS_QUERY; */
/* 	pkt_add_question(&pkt, &q->qst); */

/* 	if (q->style == QUERY_BROWSE) { */
/* 		/\* This will send at seconds 0, 1, 2, 4, 8, 16... *\/ */
/* 		tosleep = (2 << q->sent) - (1 << q->sent); */
		
/* 		if (tosleep > MAX_QUERYTIME) */
/* 			tosleep = MAX_QUERYTIME; */
/* 		timerclear(&tv); */
/* 		tv.tv_sec = tosleep; */
/* 		evtimer_add(&q->timer, &tv); */

/* 		/\* Known Answer Supression *\/ */
/* 		for (rr = cache_lookup(&q->qst.rrs); rr != NULL; */
/* 		     rr = LIST_NEXT(rr, centry)) { */
/* 			/\* Don't include packet if it's too old *\/ */
/* 			if (rr_ttl_left(rr) < rr->ttl / 2) */
/* 				continue; */
/* 			if (pkt_add_anrr(&pkt, rr) == -1) */
/* 				log_warnx("KNA error pkt_add_anrr: %s", */
/* 				    rr->rrs.dname); */
/* 		} */
/* 	} */

/* 	if (pkt_send_allif(&pkt) == -1) */
/* 		log_warnx("can't send packet to all interfaces"); */
/* 	q->sent++; */
/* } */

int
question_cmp(struct question *a, struct question *b)
{
	return (rrset_cmp(&a->rrs, &b->rrs));
}

