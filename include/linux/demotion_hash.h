#ifndef _MM_DEMOTION_HASH_H
#define _MM_DEMOTION_HASH_H

#include <linux/mm.h>

void record_demotion_history(struct folio *folio, struct lru_gen_folio *lrugen);
bool lookup_demotion_history(struct page *page,
					unsigned long *out_min_seq);
bool lookup_and_remove_demotion_history(struct page *page,
					unsigned long *out_min_seq);
bool lookup_and_remove_demotion_history2(struct folio *folio,
					unsigned long *out_min_seq);
void clear_demotion_history(void);

#endif /* _MM_DEMOTION_HASH_H */
