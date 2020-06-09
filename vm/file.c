/* file.c: Implementation of memory mapped file object (mmaped object). */

#include "vm/vm.h"
#include "filesys/file.h"

#include <string.h>
#include "threads/malloc.h"
#include "threads/mmu.h"
#include "userprog/syscall.h"

static bool file_map_swap_in (struct page *page, void *kva);
static bool file_map_swap_out (struct page *page);
static void file_map_destroy (struct page *page);

static bool file_lazy_load (struct page *page, void *aux);

/* DO NOT MODIFY this struct */
static const struct page_operations file_ops = {
	.swap_in = file_map_swap_in,
	.swap_out = file_map_swap_out,
	.destroy = file_map_destroy,
	.type = VM_FILE,
};

/* The initializer of file vm */
void
vm_file_init (void) {
}

/* Initialize the file mapped page */
bool
file_map_initializer (struct page *page, enum vm_type type, void *kva) {
	/* Set up the handler */
	page->operations = &file_ops;
	struct file_page *file_page = &page->file;
	file_page->init = page->uninit.init;
	file_page->aux = page->uninit.aux;
	file_page->type = type;
	struct lazy_aux* AUX = page->uninit.aux;
	file_page->file = AUX != NULL ? AUX->executable : NULL;
	file_page->next_page = AUX->next_page;
	file_page->swapped_out = false;
	
	return true;
}

/* Swap in the page by read contents from the file. */
static bool
file_map_swap_in (struct page *page, void *kva) {
	struct file_page *file_page = &page->file;
	if(!file_page->swapped_out){				//This page wasn't swapped out before. Finish here.
		return true;
	}
	uint8_t *kpage = kva;
	struct lazy_aux* aux = page->uninit.aux;
	size_t page_read_bytes = aux->page_read_bytes;
	size_t page_zero_bytes = aux->page_zero_bytes;
	off_t ofs = aux->offset;
	struct file* file = file_page->file;
	if (kpage == NULL)
		return false;
	file_seek(file, ofs);
	int read = file_read (file, kpage, page_read_bytes);
	if (read != (int) page_read_bytes) {
		//printf("load failed @ page 0x%X, read %d bytes instead of %d..\n", page->va, read, page_read_bytes);
		return false;
	}
	memset (kpage + page_read_bytes, 0, page_zero_bytes);
	file_page->read_bytes = page_read_bytes;
	file_page->swapped_out = false;				//This page is now not swapped-out.
	return true;
}

/* Swap out the page by writeback contents to the file. */
static bool
file_map_swap_out (struct page *page) {
	struct file_page *file_page = &page->file;
	if(pml4_is_dirty(thread_current()->pml4, page->va)){	//Write back contents to file, if DIRTY.
		off_t written = file_write_at(file_page->file, page->va, file_page->read_bytes, file_page->aux->offset);
		if((size_t) written != file_page->read_bytes){
			//printf("SWAP-OUT page 0x%X -> read_bytes : %d, offset : %d, WRITTEN : %d bytes.\n", page->va, file_page->read_bytes, file_page->aux->offset, written);
			return false;
		}
		pml4_set_dirty(thread_current()->pml4, page->va, false);
	}
	file_page->swapped_out = true;				//mark TRUE for later SWAP-IN's.
	page->frame = NULL;
	return true;
}

/* Destory the file mapped page. PAGE will be freed by the caller. */
static void
file_map_destroy (struct page *page) {
	struct file_page *file_page = &page->file;
	if(pml4_is_dirty(thread_current()->pml4, page->va)){	//Write back contents to file, if DIRTY.
		file_write_at(file_page->file, page->va, file_page->read_bytes, file_page->aux->offset);
		pml4_set_dirty(thread_current()->pml4, page->va, false);
	}
	if(file_page->aux != NULL){	//free the LAZY_AUX.
		free(file_page->aux);
	}
	if(file_page->file != NULL){	//close the file.
		file_close(file_page->file);
	}
	if(page->frame != NULL){
		vm_dealloc_frame(page->frame);
	}
	//pml4_clear_page(thread_current()->pml4, page->va);
}

//lazy-load initializer for file-mapped pages.
static bool file_lazy_load (struct page *page, void *aux) {
	uint8_t *kpage = page->frame->kva;
	size_t page_read_bytes = ((struct lazy_aux*)aux)->page_read_bytes;
	size_t page_zero_bytes = ((struct lazy_aux*)aux)->page_zero_bytes;
	off_t ofs = ((struct lazy_aux*)aux)->offset;
	struct file* file = ((struct lazy_aux*)aux)->executable;
	//printf("LOADING addr : 0x%X, read_bytes : %d, zero_bytes : %d, ofs : %d\n", page->va, page_read_bytes, page_zero_bytes, ofs);
	if (kpage == NULL)
		return false;
	// Load this page.
	file_seek(file, ofs);
	int read = file_read (file, kpage, page_read_bytes);
	if (read != (int) page_read_bytes) {
		printf("load failed @ page 0x%X, read %d bytes instead of %d..\n", page->va, read, page_read_bytes);
		return false;
	}
	memset (kpage + page_read_bytes, 0, page_zero_bytes);
	page->file.read_bytes = page_read_bytes;
	return true;
}

void *
do_mmap (void *addr, size_t length, int writable,
		struct file *file, off_t offset) {
	struct file* FILE = file;
	void* upage = addr;
	off_t ofs = offset;
	size_t read_bytes;
	//1. FAIL if addr isn't page-aligned or is 0, or Length is 0.
	if(addr == 0 || ((int)addr % PGSIZE) != 0 || !is_user_vaddr(addr) || (int) length <= 0 || ((int)offset % PGSIZE) != 0){
		//printf("FAIL @ addr : 0x%X, PGSIZE : %d, offset : %d, LENGTH : %d\n",addr, (int)addr % PGSIZE, offset, length);
		return NULL;
	}
	//2. FAIL if file length is 0.
	if(file_length(FILE) == 0){
		return NULL;
	}
	//3. Check how many pages needed & where. If any page overlaps with current spt pages, then FAIL.
	read_bytes = length <= file_length(FILE) ? length : file_length(FILE);
	int i;
	for(i=0; i < read_bytes/PGSIZE + (read_bytes % PGSIZE != 0); i++){
		if(spt_find_page(&thread_current()->spt, addr + i * PGSIZE) != NULL){
			return NULL;
		}
	}
	//4. Allocate file-mapped pages.
	while(read_bytes > 0){
		struct file* target = file_reopen(FILE);	//all PAGES get different file STRUCTS w/ file_reopen().
		size_t page_read_bytes = read_bytes < PGSIZE ? read_bytes : PGSIZE;
		size_t page_zero_bytes = PGSIZE - page_read_bytes;
		/* set up AUX. */
		void *aux = NULL;
		struct lazy_aux* AUX = malloc(sizeof(struct lazy_aux));
		AUX->executable = target;
		AUX->page_read_bytes = page_read_bytes;
		AUX->page_zero_bytes = page_zero_bytes;
		AUX->offset = ofs;
		AUX->next_page = read_bytes > PGSIZE ? true : false;
		aux = AUX;
		if (!vm_alloc_page_with_initializer (VM_FILE, upage, writable, file_lazy_load, aux)){
			return NULL;
		}
		/* Advance. */
		read_bytes -= page_read_bytes;
		upage += PGSIZE;
		ofs += PGSIZE;
	}
	return addr;
}

void
do_munmap (void *addr) {
	void* uaddr = addr;
	bool next_page;
	struct page* page = spt_find_page(&thread_current()->spt, uaddr);
	lock_acquire(&filesys_lock);
	while(page != NULL){
		if(page_get_type(page) == VM_FILE){
			struct lazy_aux* AUX = page->uninit.aux;
			next_page = AUX->next_page;
			spt_remove_page(&thread_current()->spt, page);
		}
		else{
			//printf("page addr : 0x%X is not a file-mapped page.\n", uaddr);
			break;
		}
		if(!next_page){
			break;
		}
		uaddr += PGSIZE;
		page = spt_find_page(&thread_current()->spt, uaddr);
	}
	lock_release(&filesys_lock);
}
