// https://arjunsreedharan.org/post/148675821737/memory-allocators-101-write-a-simple-memory

// The structure of a program
// Heap: Contains the dynamically allocated data.
// Stack : Contains your automatic variables, function arguments, copy of base pointer etc.

// We mainly care about heap
// The unix-based OS keeps track of the heap with sbrk()
// sbrk(0) gives the current address of program break.
// sbrk(x) with a positive value increments brk by x bytes, as a result allocating memory.
// sbrk(-x) with a negative value decrements brk by x bytes, as a result releasing memory.

// sbrk() failures return (void*) -1.

// NMAP is better but we don't really care about performance right now anyway

// The malloc(size) function allocates size bytes of memory and returns a pointer to the allocated memory.
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <pthread.h>

// void *malloc(size_t size)
// {
//     void *block;
//     block = sbrk(size);
//     if (block == (void *)-1)
//     {
//         return NULL;
//     }
//     return block;
// }

// The tricky part is freeing this memory. The free(ptr) function frees the memory block pointed to by ptr, which must have been returned by a previous call to malloc(), calloc() or realloc().
/*
But to free a block of memory, the first order of business is to know the size of the memory block to be freed. In the current scheme of things, this is not possible as the size information is not stored anywhere. So, we will have to find a way to store the size of an allocated block somewhere.
*/

// From now on, freeing a block of memory does not necessarily mean we release memory back to OS. It just means that we keep the block marked as free. This block marked as free may be reused on a later malloc() call. Since memory not at the end of the heap can’t be released, this is the only way ahead for us.
// Because releasing (back to OS) in the middle of the Heap contiguous block would be a big no-no
// So for now we just mark it as free

// Every block of allocated memory now need:
// 1. size
// 2. isFree()

// So we add them as header
struct header_t
{
    /* data */
    size_t size;
    unsigned is_free;
    struct header_t *next;
};

// Now total size = header_size + block_size
// then sbrk(total_size)
// Pointing to the next block as well
// Linked list

// The end of the header is where the actual memory block begins and therefore the memory provided to the caller by the allocator will be aligned to 16 bytes.
typedef char ALIGN[16];
union header
{
    /* data */
    struct
    {
        /* data */
        size_t size;
        unsigned is_free;
        union header *next;
    } s;
    ALIGN stub;
};

typedef union header header_t;

// Now we got head and tail of the list
header_t *head, *tail;
// To prevent two or more threads from concurrently accessing memory, we will put a basic locking mechanism in place.
// We’ll have a global lock, and before every action on memory you have to acquire the lock, and once you are done you have to release the lock.
pthread_mutex_t global_malloc_lock;

// Now our new malloc() is (after removing the old one) we can:

header_t *get_free_block(size_t size)
{
    header_t *curr = head;
    while (curr)
    {
        /* code */
        if (curr->s.is_free && curr->s.size >= size)
        {
            return curr;
        }
        curr = curr->s.next;
    }
    return NULL;
}

void *malloc(size_t size)
{
    size_t total_size;
    void *block;
    header_t *header;
    if (!size)
        return NULL;
    pthread_mutex_lock(&global_malloc_lock);
    header = get_free_block(size);
    if (header)
    {
        header->s.is_free = 0;
        pthread_mutex_unlock(&global_malloc_lock);
        return (void *)(header + 1);
    }
    total_size = sizeof(header_t) + size;
    block = sbrk(total_size);
    if (block == (void *)-1)
    {
        pthread_mutex_unlock(&global_malloc_lock);
        return NULL;
    }
    header = block;
    header->s.size = size;
    header->s.is_free = 0;
    header->s.next = NULL;
    if (!head)
        head = header;
    if (tail)
        tail->s.next = header;

    tail = header;
    pthread_mutex_unlock(&global_malloc_lock);
    return (void *)(header + 1);
}
/*
We check if the requested size is zero. If it is, then we return NULL.
For a valid size, we first acquire the lock. The we call get_free_block() - it traverses the linked list and see if there already exist a block of memory that is marked as free and can accomodate the given size. Here, we take a first-fit approach in searching the linked list.

If a sufficiently large free block is found, we will simply mark that block as not-free, release the global lock, and then return a pointer to that block. In such a case, the header pointer will refer to the header part of the block of memory we just found by traversing the list. Remember, we have to hide the very existence of the header to an outside party. When we do (header + 1), it points to the byte right after the end of the header. This is incidentally also the first byte of the actual memory block, the one the caller is interested in. This is cast to (void*) and returned.

If we have not found a sufficiently large free block, then we have to extend the heap by calling sbrk(). The heap has to be extended by a size that fits the requested size as well a header. For that, we first compute the total size: total_size = sizeof(header_t) + size;. Now, we request the OS to increment the program break: sbrk(total_size).

In the memory thus obtained from the OS, we first make space for the header. In C, there is no need to cast a void* to any other pointer type, it is always safely promoted. That’s why we don’t explicitly do: header = (header_t *)block;
We fill this header with the requested size (not the total size) and mark it as not-free. We update the next pointer, head and tail so to reflect the new state of the linked list. As explained earlier, we hide the header from the caller and hence return (void*)(header + 1). We make sure we release the global lock as well.
*/

void free(void *block)
{
    header_t *header, *tmp;
    void *programbreak;
    if (!block)
    {
        return;
    }
    pthread_mutex_lock(&global_malloc_lock);
    header = (header_t *)block - 1;

    programbreak = sbrk(0);
    if ((char *)block + header->s.size == programbreak)
    {
        if (head == tail)
        {
            head = tail = NULL;
        }
        else
        {
            tmp = head;
            while (tmp)
            {
                if (tmp->s.next == tail)
                {
                    tmp->s.next = NULL;
                    tail = tmp;
                }
                tmp = tmp->s.next;
            }
        }
        sbrk(0 - sizeof(header_t) - header->s.size);
        pthread_mutex_unlock(&global_malloc_lock);
        return;
    }
    header->s.is_free = 1;
    pthread_mutex_unlock(&global_malloc_lock);
}

/*
Here, first we get the header of the block we want to free. All we need to do is get a pointer that is behind the block by a distance equalling the size of the header. So, we cast block to a header pointer type and move it behind by 1 unit.
header = (header_t*)block - 1;

sbrk(0) gives the current value of program break. To check if the block to be freed is at the end of the heap, we first find the end of the current block. The end can be computed as (char*)block + header->s.size. This is then compared with the program break.

If it is in fact at the end, then we could shrink the size of the heap and release memory to OS. We first reset our head and tail pointers to reflect the loss of the last block. Then the amount of memory to be released is calculated. This the sum of sizes of the header and the acutal block: sizeof(header_t) + header->s.size. To release this much amount of memory, we call sbrk() with the negative of this value.

In the case the block is not the last one in the linked list, we simply set the is_free field of its header. This is the field checked by get_free_block() before actually calling sbrk() on a malloc().
*/
void *calloc(size_t num, size_t nsize)
{
    size_t size;
    void *block;
    if (!num || !nsize)
    {
        return NULL;
    }
    size = num * nsize;
    /* Check overflow */
    if (nsize != size / num)
    {
        return NULL;
    }
    block = malloc(size);
    if (!block)
    {
        return NULL;
    }
    memset(block, 0, size);
    return block;
}

void *realloc(void *block, size_t size)
{
    header_t *header;
    void *ret;
    if (!block || !size)
        return malloc(size);
    header = (header_t *)block - 1;
    if (header->s.size >= size)
        return block;
    ret = malloc(size);
    if (ret)
    {
        memcpy(ret, block, header->s.size);
        free(block);
    }
    return ret;
}

/* A debug function to print the entire link list */
void print_mem_list()
{
    header_t *curr = head;
    printf("head = %p, tail = %p \n", (void *)head, (void *)tail);
    while (curr)
    {
        printf("addr = %p, size = %zu, is_free=%u, next=%p\n",
               (void *)curr, curr->s.size, curr->s.is_free, (void *)curr->s.next);
        curr = curr->s.next;
    }
}
