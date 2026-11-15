// SPDX-License-Identifier: BSD-3-Clause

#define _DEFAULT_SOURCE
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include "block_meta.h"
#include "osmem.h"

#ifndef MMAP_THRESHOLD
#define MMAP_THRESHOLD (128 * 1024)
#endif

#define ALIGNMENT 8
#define ALIGN(size) (((size) + (ALIGNMENT - 1)) & ~(ALIGNMENT - 1))
#define META_SIZE ALIGN(sizeof(struct block_meta))
#define MIN_SPLIT_SIZE (META_SIZE + ALIGN(1))

static struct block_meta *heap_head;
static int heap_preallocated;

static void *payload_from_block(struct block_meta *block)
{
	return (void *)((char *)block + META_SIZE);
}

static struct block_meta *block_from_payload(void *ptr)
{
	return (struct block_meta *)((char *)ptr - META_SIZE);
}

/* Розділяє вільний або зайнятий блок, якщо залишок вміщує заголовок + щонайменше 8 байт даних */
static void split_block(struct block_meta *block, size_t aligned_size)
{
	if (block->size < aligned_size + MIN_SPLIT_SIZE)
		return;

	struct block_meta *rem = (struct block_meta *)((char *)block + META_SIZE + aligned_size);

	rem->size = block->size - aligned_size - META_SIZE;
	rem->status = STATUS_FREE;
	rem->prev = block;
	rem->next = block->next;

	if (block->next)
		block->next->prev = rem;

	block->next = rem;
	block->size = aligned_size;
}

/* Об'єднує сусідні вільні блоки в купі (heap) */
static void coalesce_free_blocks(void)
{
	struct block_meta *curr = heap_head;

	while (curr && curr->next) {
		if (curr->status == STATUS_FREE && curr->next->status == STATUS_FREE) {
			struct block_meta *nxt = curr->next;

			curr->size += META_SIZE + nxt->size;
			curr->next = nxt->next;
			if (nxt->next)
				nxt->next->prev = curr;
		} else {
			curr = curr->next;
		}
	}
}

/* Пошук найкращого вільного блоку за стратегією Best-Fit */
static struct block_meta *find_best_fit(size_t aligned_size)
{
	struct block_meta *curr = heap_head;
	struct block_meta *best = NULL;

	while (curr) {
		if (curr->status == STATUS_FREE && curr->size >= aligned_size) {
			if (!best || curr->size < best->size)
				best = curr;
		}
		curr = curr->next;
	}

	return best;
}

/* Повертає останній блок у списку купи */
static struct block_meta *get_heap_tail(void)
{
	struct block_meta *curr = heap_head;

	if (!curr)
		return NULL;

	while (curr->next)
		curr = curr->next;

	return curr;
}

/* Виділення блоку через mmap (для великих запитів) */
static void *alloc_mapped(size_t aligned_size)
{
	size_t total_size = META_SIZE + aligned_size;
	void *raw = mmap(NULL, total_size, PROT_READ | PROT_WRITE,
			 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

	DIE(raw == MAP_FAILED, "mmap");

	struct block_meta *block = (struct block_meta *)raw;

	block->size = aligned_size;
	block->status = STATUS_MAPPED;
	block->prev = NULL;
	block->next = NULL;

	return payload_from_block(block);
}

/* Виділення блоку в купі через sbrk (із попередньою алокацією 128 КБ) */
static void *alloc_heap(size_t aligned_size)
{
	if (!heap_preallocated) {
		void *raw = sbrk(MMAP_THRESHOLD);

		DIE(raw == (void *)-1, "sbrk");

		struct block_meta *initial = (struct block_meta *)raw;

		initial->size = MMAP_THRESHOLD - META_SIZE;
		initial->status = STATUS_FREE;
		initial->prev = NULL;
		initial->next = NULL;

		heap_head = initial;
		heap_preallocated = 1;
	}

	coalesce_free_blocks();

	struct block_meta *best = find_best_fit(aligned_size);

	if (best) {
		split_block(best, aligned_size);
		best->status = STATUS_ALLOC;
		return payload_from_block(best);
	}

	struct block_meta *tail = get_heap_tail();

	/* Якщо останній блок у купі вільний — розширюємо його замість створення нового */
	if (tail && tail->status == STATUS_FREE) {
		size_t needed = aligned_size - tail->size;
		void *ext = sbrk((intptr_t)needed);

		DIE(ext == (void *)-1, "sbrk");

		tail->size = aligned_size;
		tail->status = STATUS_ALLOC;
		return payload_from_block(tail);
	}

	/* Інакше додаємо новий блок у кінець купи */
	size_t total_size = META_SIZE + aligned_size;
	void *raw = sbrk((intptr_t)total_size);

	DIE(raw == (void *)-1, "sbrk");

	struct block_meta *block = (struct block_meta *)raw;

	block->size = aligned_size;
	block->status = STATUS_ALLOC;
	block->prev = tail;
	block->next = NULL;

	if (tail)
		tail->next = block;
	else
		heap_head = block;

	return payload_from_block(block);
}

void *os_malloc(size_t size)
{
	if (size == 0)
		return NULL;

	size_t aligned_size = ALIGN(size);

	if (aligned_size < size)
		return NULL;

	if (META_SIZE + aligned_size >= MMAP_THRESHOLD)
		return alloc_mapped(aligned_size);

	return alloc_heap(aligned_size);
}

void os_free(void *ptr)
{
	if (!ptr)
		return;

	struct block_meta *block = block_from_payload(ptr);

	if (block->status == STATUS_MAPPED) {
		int ret = munmap(block, META_SIZE + block->size);

		DIE(ret == -1, "munmap");
		return;
	}

	if (block->status == STATUS_ALLOC) {
		block->status = STATUS_FREE;

		/* Коалесценція з наступним вільним блоком */
		if (block->next && block->next->status == STATUS_FREE) {
			struct block_meta *nxt = block->next;

			block->size += META_SIZE + nxt->size;
			block->next = nxt->next;
			if (nxt->next)
				nxt->next->prev = block;
		}

		/* Коалесценція з попереднім вільним блоком */
		if (block->prev && block->prev->status == STATUS_FREE) {
			struct block_meta *prv = block->prev;

			prv->size += META_SIZE + block->size;
			prv->next = block->next;
			if (block->next)
				block->next->prev = prv;
		}
	}
}

void *os_calloc(size_t nmemb, size_t size)
{
	if (nmemb == 0 || size == 0)
		return NULL;

	if (nmemb > (size_t)-1 / size)
		return NULL;

	size_t raw_size = nmemb * size;
	size_t aligned_size = ALIGN(raw_size);

	if (aligned_size < raw_size)
		return NULL;

	long page_size = sysconf(_SC_PAGESIZE);

	DIE(page_size <= 0, "sysconf");

	void *ptr;

	if (META_SIZE + aligned_size >= (size_t)page_size)
		ptr = alloc_mapped(aligned_size);
	else
		ptr = alloc_heap(aligned_size);

	if (ptr)
		memset(ptr, 0, aligned_size);

	return ptr;
}

void *os_realloc(void *ptr, size_t size)
{
	if (!ptr)
		return os_malloc(size);

	if (size == 0) {
		os_free(ptr);
		return NULL;
	}

	struct block_meta *block = block_from_payload(ptr);

	if (block->status == STATUS_FREE)
		return NULL;

	size_t aligned_size = ALIGN(size);

	if (aligned_size < size)
		return NULL;

	size_t total_size = META_SIZE + aligned_size;

	if (block->status == STATUS_MAPPED || total_size >= MMAP_THRESHOLD) {
		void *new_ptr = os_malloc(size);

		if (!new_ptr)
			return NULL;

		size_t copy_size = block->size < aligned_size ? block->size : aligned_size;

		memcpy(new_ptr, ptr, copy_size);
		os_free(ptr);
		return new_ptr;
	}

	/* Зменшення розміру без поділу блоку */
	if (aligned_size <= block->size)
		return ptr;

	size_t old_size = block->size;

	/* Розширення на місці шляхом поглинання сусідніх вільних блоків */
	while (block->next && block->next->status == STATUS_FREE && block->size < aligned_size) {
		struct block_meta *nxt = block->next;

		block->size += META_SIZE + nxt->size;
		block->next = nxt->next;
		if (nxt->next)
			nxt->next->prev = block;
	}

	if (block->size >= aligned_size)
		return ptr;

	/* Розширення останнього блоку в купі через sbrk */
	if (!block->next) {
		size_t needed = aligned_size - block->size;
		void *ext = sbrk((intptr_t)needed);

		DIE(ext == (void *)-1, "sbrk");

		block->size = aligned_size;
		return ptr;
	}

	void *new_ptr = os_malloc(size);

	if (!new_ptr)
		return NULL;

	memcpy(new_ptr, ptr, old_size);
	os_free(ptr);
	return new_ptr;
}