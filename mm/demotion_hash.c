#include <linux/demotion_hash.h>
#include <linux/hashtable.h>
#include <linux/hash.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/mm_inline.h>
#include <linux/sysfs.h>
#include <linux/kobject.h>

// total # of entry: # of buckets(2^DEMOTION_HASH_BITS) * MAX_BUCKET_DEPTH = 2^18개 페이지(1GB)에 대한 정보 유지
// total size = 40B * 2^16 = 약 8MB
struct demotion_history {
	struct address_space *mapping;
	pgoff_t index;
	unsigned long min_seq;
	struct hlist_node node;
};

// # of buckets = 2^16
#define DEMOTION_HASH_BITS 16
static DEFINE_HASHTABLE(demotion_htable, DEMOTION_HASH_BITS);
static DEFINE_SPINLOCK(demotion_htable_lock);

static atomic_t demotion_entries = ATOMIC_INIT(0);
// max # of entry per bucket
#define MAX_BUCKET_DEPTH 4

unsigned long demotion_max_entries = (1UL << (DEMOTION_HASH_BITS + 2));
unsigned long demotion_high_watermark =
	((1UL << (DEMOTION_HASH_BITS + 2)) * 3 / 4);

// 1/8 sample rate
#define DEMOTION_SAMPLE_RATE 8
static atomic_t demotion_sample_counter = ATOMIC_INIT(0);

static void shrink_demotion_history(void);

static ssize_t show_demotion_max_entries(struct kobject *kobj,
					 struct kobj_attribute *attr, char *buf)
{
	return sprintf(buf, "%lu\n", READ_ONCE(demotion_max_entries));
}

static ssize_t store_demotion_max_entries(struct kobject *kobj,
					  struct kobj_attribute *attr,
					  const char *buf, size_t len)
{
	unsigned long new_max_entries;
	int ret;

	ret = kstrtoul(buf, 10, &new_max_entries);
	if (ret)
		return ret;

	if (new_max_entries < 1024)
		new_max_entries = 1024;
	WRITE_ONCE(demotion_max_entries, new_max_entries);
	WRITE_ONCE(demotion_high_watermark, (new_max_entries * 3 / 4));

	while (atomic_read(&demotion_entries) >=
	       READ_ONCE(demotion_high_watermark)) {
		shrink_demotion_history();
	}
	return len;
}

struct kobj_attribute demotion_max_entries_attr =
	__ATTR(demotion_max_entries, 0644, show_demotion_max_entries,
	       store_demotion_max_entries);

static inline unsigned long get_folio_hash_key(struct address_space *mapping,
					       pgoff_t index)
{
	return hash_ptr(mapping, DEMOTION_HASH_BITS) ^
	       hash_long(index, DEMOTION_HASH_BITS);
}

void record_demotion_history(struct folio *folio, struct lru_gen_folio *lrugen)
{
	struct demotion_history *entry, *oldest = NULL;
	struct demotion_history *new_entry;
	unsigned long hash_key;
	int type, depth = 0;

	if (atomic_inc_return(&demotion_sample_counter) %
		    DEMOTION_SAMPLE_RATE !=
	    0)
		return;

	if (atomic_read(&demotion_entries) >=
	    READ_ONCE(demotion_high_watermark))
		shrink_demotion_history();

	new_entry = kmalloc(sizeof(*new_entry), GFP_NOWAIT | __GFP_NOWARN);
	if (!new_entry)
		return;

	type = folio_is_file_lru(folio);
	new_entry->mapping = folio->mapping;
	new_entry->index = folio->index;
	new_entry->min_seq = READ_ONCE(lrugen->min_seq[type]);

	hash_key = get_folio_hash_key(folio->mapping, folio->index);

	spin_lock(&demotion_htable_lock);

	hlist_for_each_entry(
		entry, &demotion_htable[hash_min(hash_key, DEMOTION_HASH_BITS)],
		node) {
		// 동일한 페이지 강등 시, min_seq만업데이트
		if (entry->mapping == folio->mapping &&
		    entry->index == folio->index) {
			entry->min_seq = new_entry->min_seq;
			spin_unlock(&demotion_htable_lock);
			kfree(new_entry);
			return;
		}

		depth++;
		oldest = entry;
	}

	// 버킷이 꽉찬경우, tail node 삭제
	if (depth >= MAX_BUCKET_DEPTH && oldest) {
		hash_del(&oldest->node);
		kfree(oldest);
	}

	// 새 페이지의 강등 정보를 추가
	hash_add(demotion_htable, &new_entry->node, hash_key);
	atomic_inc(&demotion_entries);

	spin_unlock(&demotion_htable_lock);
}

bool lookup_demotion_history(struct page *page, unsigned long *out_min_seq)
{
	struct folio *folio = page_folio(page);
	struct demotion_history *entry;
	unsigned long hash_key =
		get_folio_hash_key(folio->mapping, folio->index);
	bool found = false;

	spin_lock(&demotion_htable_lock);

	hash_for_each_possible(demotion_htable, entry, node, hash_key) {
		if (entry->mapping == folio->mapping &&
		    entry->index == folio->index) {
			*out_min_seq = entry->min_seq;
			found = true;
			break;
		}
	}

	spin_unlock(&demotion_htable_lock);
	return found;
}

bool lookup_and_remove_demotion_history(struct page *page,
					unsigned long *out_min_seq)
{
	struct folio *folio = page_folio(page);
	struct demotion_history *entry;
	struct hlist_node *tmp;
	unsigned long hash_key =
		get_folio_hash_key(folio->mapping, folio->index);
	bool found = false;

	spin_lock(&demotion_htable_lock);

	/* hash_for_each_possible_safe를 사용하여 순회 중 삭제(hash_del)해도 안전하게 구현 */
	hash_for_each_possible_safe(demotion_htable, entry, tmp, node,
				    hash_key) {
		/* 해시 충돌을 대비해 실제 mapping과 index가 정확히 일치하는지 확인 */
		if (entry->mapping == folio->mapping &&
		    entry->index == folio->index) {
			*out_min_seq = entry->min_seq;

			hash_del(&entry->node);
			kfree(entry);
			atomic_dec(&demotion_entries);

			found = true;
			break;
		}
	}

	spin_unlock(&demotion_htable_lock);
	return found;
}

bool lookup_and_remove_demotion_history2(struct folio *folio,
					 unsigned long *out_min_seq)
{
	struct demotion_history *entry;
	struct hlist_node *tmp;
	unsigned long hash_key =
		get_folio_hash_key(folio->mapping, folio->index);
	bool found = false;

	spin_lock(&demotion_htable_lock);

	/* hash_for_each_possible_safe를 사용하여 순회 중 삭제(hash_del)해도 안전하게 구현 */
	hash_for_each_possible_safe(demotion_htable, entry, tmp, node,
				    hash_key) {
		/* 해시 충돌을 대비해 실제 mapping과 index가 정확히 일치하는지 확인 */
		if (entry->mapping == folio->mapping &&
		    entry->index == folio->index) {
			*out_min_seq = entry->min_seq;

			hash_del(&entry->node);
			kfree(entry);
			atomic_dec(&demotion_entries);

			found = true;
			break;
		}
	}

	spin_unlock(&demotion_htable_lock);
	return found;
}

void clear_demotion_history(void)
{
	struct demotion_history *entry;
	struct hlist_node *tmp;
	int bucket;

	spin_lock(&demotion_htable_lock);

	hash_for_each_safe(demotion_htable, bucket, tmp, entry, node) {
		hash_del(&entry->node);
		kfree(entry);
		atomic_dec(&demotion_entries);
	}

	spin_unlock(&demotion_htable_lock);
}

static void shrink_demotion_history(void)
{
	struct demotion_history *entry;
	struct hlist_node *tmp;
	int bucket, count = 0;

	spin_lock(&demotion_htable_lock);

	hash_for_each_safe(demotion_htable, bucket, tmp, entry, node) {
		hash_del(&entry->node);
		kfree(entry);
		atomic_dec(&demotion_entries);

		if (++count >= 1024)
			break;
	}

	spin_unlock(&demotion_htable_lock);
}
