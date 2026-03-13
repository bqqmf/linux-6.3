#ifndef _MM_DEMOTION_HASH_H
#define _MM_DEMOTION_HASH_H

#include <linux/mm.h>

void record_demotion_history(struct folio *folio, struct lru_gen_folio *lrugen);
bool lookup_and_remove_demotion_history(struct page *page, unsigned long *out_min_seq);

#endif /* _MM_DEMOTION_HASH_H */
