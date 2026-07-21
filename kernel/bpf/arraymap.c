/* Copyright (c) 2011-2014 PLUMgrid, http://plumgrid.com
 *
 * Minimal BPF_MAP_TYPE_ARRAY for Linux 3.18. Xiaomi gemini (4.4) has a fuller
 * eBPF stack; this provides create/lookup/update for AOSP bpfloader maps.
 */
#include <linux/bpf.h>
#include <linux/err.h>
#include <linux/string.h>
#include <linux/slab.h>
#include <linux/mm.h>
#include <linux/filter.h>
#include <linux/vmalloc.h>

struct bpf_array {
	struct bpf_map map;
	u32 elem_size;
	/* elems starts here: max_entries * elem_size bytes */
	char elems[0] __aligned(8);
};

static struct bpf_map *array_map_alloc(union bpf_attr *attr)
{
	struct bpf_array *array;
	u64 array_size;
	u32 elem_size;

	if (attr->max_entries == 0 || attr->key_size != 4 ||
	    attr->value_size == 0)
		return ERR_PTR(-EINVAL);

	elem_size = round_up(attr->value_size, 8);

	array_size = sizeof(*array);
	array_size += (u64)attr->max_entries * elem_size;
	if (array_size >= U32_MAX - PAGE_SIZE)
		return ERR_PTR(-ENOMEM);

	array = kzalloc(array_size, GFP_USER | __GFP_NOWARN);
	if (!array) {
		array = vzalloc(array_size);
		if (!array)
			return ERR_PTR(-ENOMEM);
	}

	array->map.key_size = attr->key_size;
	array->map.value_size = attr->value_size;
	array->map.max_entries = attr->max_entries;
	array->elem_size = elem_size;

	return &array->map;
}

static void array_map_free(struct bpf_map *map)
{
	struct bpf_array *array = container_of(map, struct bpf_array, map);

	synchronize_rcu();
	if (is_vmalloc_addr(array))
		vfree(array);
	else
		kfree(array);
}

static void *array_map_lookup_elem(struct bpf_map *map, void *key)
{
	struct bpf_array *array = container_of(map, struct bpf_array, map);
	u32 index = *(u32 *)key;

	if (unlikely(index >= array->map.max_entries))
		return NULL;

	return array->elems + array->elem_size * index;
}

static int array_map_get_next_key(struct bpf_map *map, void *key, void *next_key)
{
	struct bpf_array *array = container_of(map, struct bpf_array, map);
	u32 index = *(u32 *)key;
	u32 *next = next_key;

	if (index >= array->map.max_entries) {
		*next = 0;
		return 0;
	}

	if (index == array->map.max_entries - 1)
		return -ENOENT;

	*next = index + 1;
	return 0;
}

static int array_map_update_elem(struct bpf_map *map, void *key, void *value)
{
	struct bpf_array *array = container_of(map, struct bpf_array, map);
	u32 index = *(u32 *)key;

	if (unlikely(index >= array->map.max_entries))
		return -E2BIG;

	memcpy(array->elems + array->elem_size * index, value, map->value_size);
	return 0;
}

static int array_map_delete_elem(struct bpf_map *map, void *key)
{
	return -EINVAL;
}

static struct bpf_map_ops array_ops = {
	.map_alloc = array_map_alloc,
	.map_free = array_map_free,
	.map_get_next_key = array_map_get_next_key,
	.map_lookup_elem = array_map_lookup_elem,
	.map_update_elem = array_map_update_elem,
	.map_delete_elem = array_map_delete_elem,
};

static struct bpf_map_type_list array_type = {
	.ops = &array_ops,
	.type = BPF_MAP_TYPE_ARRAY,
};

static int __init register_array_map(void)
{
	bpf_register_map_type(&array_type);
	return 0;
}
late_initcall(register_array_map);
