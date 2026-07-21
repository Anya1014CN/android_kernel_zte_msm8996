/* Copyright (c) 2011-2014 PLUMgrid, http://plumgrid.com
 *
 * Minimal BPF_MAP_TYPE_HASH for Linux 3.18. Gemini/4.4 has a richer hashtab;
 * this covers create/lookup/update/delete for userspace map creation.
 */
#include <linux/bpf.h>
#include <linux/jhash.h>
#include <linux/slab.h>
#include <linux/err.h>
#include <linux/string.h>
#include <linux/filter.h>
#include <linux/vmalloc.h>
#include <linux/rculist.h>
#include <linux/spinlock.h>
#include <linux/log2.h>

struct htab_elem {
	struct hlist_node hash_node;
	struct rcu_head rcu;
	u32 hash;
	char key[0] __aligned(8);
};

struct bpf_htab {
	struct bpf_map map;
	struct hlist_head *buckets;
	spinlock_t *locks;
	u32 n_buckets;
	u32 elem_size;
	atomic_t count;
};

static inline struct hlist_head *select_bucket(struct bpf_htab *htab, u32 hash)
{
	return &htab->buckets[hash & (htab->n_buckets - 1)];
}

static inline spinlock_t *select_lock(struct bpf_htab *htab, u32 hash)
{
	return &htab->locks[hash & (htab->n_buckets - 1)];
}

static u32 htab_map_hash(const void *key, u32 key_size)
{
	return jhash(key, key_size, 0);
}

static struct htab_elem *lookup_elem_raw(struct hlist_head *head, u32 hash,
					 void *key, u32 key_size)
{
	struct htab_elem *l;

	hlist_for_each_entry_rcu(l, head, hash_node)
		if (l->hash == hash && !memcmp(&l->key, key, key_size))
			return l;

	return NULL;
}

static struct bpf_map *htab_map_alloc(union bpf_attr *attr)
{
	struct bpf_htab *htab;
	int err = -EINVAL, i;
	u64 cost;

	if (attr->max_entries == 0 || attr->key_size == 0 ||
	    attr->value_size == 0)
		return ERR_PTR(-EINVAL);

	if (attr->key_size > MAX_BPF_STACK ||
	    attr->value_size > KMALLOC_MAX_SIZE)
		return ERR_PTR(-E2BIG);

	htab = kzalloc(sizeof(*htab), GFP_USER);
	if (!htab)
		return ERR_PTR(-ENOMEM);

	htab->map.key_size = attr->key_size;
	htab->map.value_size = attr->value_size;
	htab->map.max_entries = attr->max_entries;

	htab->n_buckets = roundup_pow_of_two(attr->max_entries);
	if (htab->n_buckets < 2)
		htab->n_buckets = 2;

	htab->elem_size = sizeof(struct htab_elem) +
		round_up(attr->key_size, 8) +
		round_up(attr->value_size, 8);

	cost = (u64)htab->n_buckets *
		(sizeof(struct hlist_head) + sizeof(spinlock_t));
	cost += (u64)htab->elem_size * attr->max_entries;
	if (cost >= U32_MAX - PAGE_SIZE) {
		err = -ENOMEM;
		goto free_htab;
	}

	htab->buckets = kcalloc(htab->n_buckets, sizeof(struct hlist_head),
				 GFP_USER | __GFP_NOWARN);
	if (!htab->buckets) {
		htab->buckets = vzalloc(htab->n_buckets *
					  sizeof(struct hlist_head));
		if (!htab->buckets) {
			err = -ENOMEM;
			goto free_htab;
		}
	}

	htab->locks = kcalloc(htab->n_buckets, sizeof(spinlock_t),
			      GFP_USER | __GFP_NOWARN);
	if (!htab->locks) {
		htab->locks = vmalloc(htab->n_buckets * sizeof(spinlock_t));
		if (!htab->locks) {
			err = -ENOMEM;
			goto free_buckets;
		}
		memset(htab->locks, 0, htab->n_buckets * sizeof(spinlock_t));
	}

	for (i = 0; i < htab->n_buckets; i++)
		spin_lock_init(&htab->locks[i]);

	atomic_set(&htab->count, 0);
	return &htab->map;

free_buckets:
	if (is_vmalloc_addr(htab->buckets))
		vfree(htab->buckets);
	else
		kfree(htab->buckets);
free_htab:
	kfree(htab);
	return ERR_PTR(err);
}

static void htab_elem_free_rcu(struct rcu_head *head)
{
	struct htab_elem *l = container_of(head, struct htab_elem, rcu);

	kfree(l);
}

static void htab_map_free(struct bpf_map *map)
{
	struct bpf_htab *htab = container_of(map, struct bpf_htab, map);
	struct hlist_head *head;
	struct hlist_node *n;
	struct htab_elem *l;
	int i;

	synchronize_rcu();

	for (i = 0; i < htab->n_buckets; i++) {
		head = &htab->buckets[i];
		hlist_for_each_entry_safe(l, n, head, hash_node) {
			hlist_del_rcu(&l->hash_node);
			kfree(l);
		}
	}

	if (is_vmalloc_addr(htab->buckets))
		vfree(htab->buckets);
	else
		kfree(htab->buckets);

	if (is_vmalloc_addr(htab->locks))
		vfree(htab->locks);
	else
		kfree(htab->locks);

	kfree(htab);
}

static void *htab_map_lookup_elem(struct bpf_map *map, void *key)
{
	struct bpf_htab *htab = container_of(map, struct bpf_htab, map);
	struct hlist_head *head;
	struct htab_elem *l;
	u32 hash, key_size;

	key_size = map->key_size;
	hash = htab_map_hash(key, key_size);
	head = select_bucket(htab, hash);
	l = lookup_elem_raw(head, hash, key, key_size);
	if (l)
		return l->key + round_up(map->key_size, 8);

	return NULL;
}

static int htab_map_get_next_key(struct bpf_map *map, void *key, void *next_key)
{
	struct bpf_htab *htab = container_of(map, struct bpf_htab, map);
	struct hlist_head *head;
	struct htab_elem *l, *next_l;
	struct hlist_node *node;
	u32 hash, key_size;
	int i;

	key_size = map->key_size;
	hash = htab_map_hash(key, key_size);
	head = select_bucket(htab, hash);

	l = lookup_elem_raw(head, hash, key, key_size);
	if (l) {
		node = rcu_dereference_raw(hlist_next_rcu(&l->hash_node));
		if (node) {
			next_l = hlist_entry(node, struct htab_elem, hash_node);
			memcpy(next_key, next_l->key, key_size);
			return 0;
		}
		i = (hash & (htab->n_buckets - 1)) + 1;
	} else {
		i = 0;
	}

	for (; i < htab->n_buckets; i++) {
		head = &htab->buckets[i];
		node = rcu_dereference_raw(hlist_first_rcu(head));
		if (node) {
			next_l = hlist_entry(node, struct htab_elem, hash_node);
			memcpy(next_key, next_l->key, key_size);
			return 0;
		}
	}

	return -ENOENT;
}

static int htab_map_update_elem(struct bpf_map *map, void *key, void *value)
{
	struct bpf_htab *htab = container_of(map, struct bpf_htab, map);
	struct htab_elem *l_new, *l_old;
	struct hlist_head *head;
	spinlock_t *lock;
	u32 key_size, hash;
	unsigned long flags;

	key_size = map->key_size;
	hash = htab_map_hash(key, key_size);
	head = select_bucket(htab, hash);
	lock = select_lock(htab, hash);

	l_new = kmalloc(htab->elem_size, GFP_ATOMIC | __GFP_NOWARN);
	if (!l_new)
		return -ENOMEM;

	memcpy(l_new->key, key, key_size);
	memcpy(l_new->key + round_up(key_size, 8), value, map->value_size);
	l_new->hash = hash;

	spin_lock_irqsave(lock, flags);
	l_old = lookup_elem_raw(head, hash, key, key_size);
	if (!l_old && atomic_read(&htab->count) >= map->max_entries) {
		spin_unlock_irqrestore(lock, flags);
		kfree(l_new);
		return -E2BIG;
	}

	hlist_add_head_rcu(&l_new->hash_node, head);
	if (l_old) {
		hlist_del_rcu(&l_old->hash_node);
		call_rcu(&l_old->rcu, htab_elem_free_rcu);
	} else {
		atomic_inc(&htab->count);
	}
	spin_unlock_irqrestore(lock, flags);
	return 0;
}

static int htab_map_delete_elem(struct bpf_map *map, void *key)
{
	struct bpf_htab *htab = container_of(map, struct bpf_htab, map);
	struct hlist_head *head;
	spinlock_t *lock;
	struct htab_elem *l;
	unsigned long flags;
	u32 hash, key_size;
	int ret = -ENOENT;

	key_size = map->key_size;
	hash = htab_map_hash(key, key_size);
	head = select_bucket(htab, hash);
	lock = select_lock(htab, hash);

	spin_lock_irqsave(lock, flags);
	l = lookup_elem_raw(head, hash, key, key_size);
	if (l) {
		hlist_del_rcu(&l->hash_node);
		atomic_dec(&htab->count);
		call_rcu(&l->rcu, htab_elem_free_rcu);
		ret = 0;
	}
	spin_unlock_irqrestore(lock, flags);
	return ret;
}

static struct bpf_map_ops htab_ops = {
	.map_alloc = htab_map_alloc,
	.map_free = htab_map_free,
	.map_get_next_key = htab_map_get_next_key,
	.map_lookup_elem = htab_map_lookup_elem,
	.map_update_elem = htab_map_update_elem,
	.map_delete_elem = htab_map_delete_elem,
};

static struct bpf_map_type_list htab_type = {
	.ops = &htab_ops,
	.type = BPF_MAP_TYPE_HASH,
};

static int __init register_htab_map(void)
{
	bpf_register_map_type(&htab_type);
	return 0;
}
late_initcall(register_htab_map);
