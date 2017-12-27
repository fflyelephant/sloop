#ifndef _DLIST_H_
#define _DLIST_H_

typedef struct dlist_head dlist_t;
struct dlist_head
{
	struct dlist_head * next;
	struct dlist_head * prev;
};

#define DLIST_HEAD_INIT(e)	{&(e),&(e)}
#define DLIST_HEAD(name) 	dlist_t name = {&(name),&(name)}
#define INIT_DLIST_HEAD(e)	do { (e)->next = (e)->prev = (e); } while (0)

static inline void __dlist_add(dlist_t * entry, dlist_t * prev, dlist_t * next)
{
	next->prev = entry;
	entry->next = next;
	entry->prev = prev;
	prev->next = entry;
}
static inline void __dlist_del(dlist_t * prev, dlist_t * next)
{
	next->prev = prev;
	prev->next = next;
}

/***************************************************************************/

#define dlist_entry(e, t, m) ((t *)((char *)(e)-(unsigned long)(&((t *)0)->m)))

static inline int  dlist_empty(struct dlist_head * head)			{ return head->next == head; }
static inline void dlist_add(dlist_t * entry, dlist_t * head)		{ __dlist_add(entry, head, head->next); }
static inline void dlist_add_tail(dlist_t * entry, dlist_t * head)	{ __dlist_add(entry, head->prev, head); }
static inline void dlist_del(dlist_t * entry)
{
	__dlist_del(entry->prev, entry->next);
	entry->next = entry->prev = (void *)0;
}
static inline void dlist_del_init(dlist_t * entry)
{
	__dlist_del(entry->prev, entry->next);
	entry->next = entry->prev = entry;
}
static inline dlist_t * dlist_get_next(dlist_t * entry, dlist_t * head)
{
	entry = entry ? entry->next : head->next;
	return (entry == head) ? NULL : entry;
}
static inline dlist_t * dlist_get_prev(dlist_t * entry, dlist_t * head)
{
	entry = entry ? entry->prev : head->prev;
	return (entry == head) ? NULL : entry;
}

#endif
