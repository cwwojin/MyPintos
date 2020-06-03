/* file.c: Implementation of memory mapped file object (mmaped object). */

#include "vm/vm.h"
#include "filesys/file.h"

static bool file_map_swap_in (struct page *page, void *kva);
static bool file_map_swap_out (struct page *page);
static void file_map_destroy (struct page *page);

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
	file_page->type = page->uninit.type;
	struct lazy_aux* AUX = page->uninit.aux;
	file_page->file = AUX != NULL ? AUX->executable : NULL;
	file_page->next_page = AUX->next_page;
}

/* Swap in the page by read contents from the file. */
static bool
file_map_swap_in (struct page *page, void *kva) {
	struct file_page *file_page UNUSED = &page->file;
}

/* Swap out the page by writeback contents to the file. */
static bool
file_map_swap_out (struct page *page) {
	struct file_page *file_page UNUSED = &page->file;
}

/* Destory the file mapped page. PAGE will be freed by the caller. */
static void
file_map_destroy (struct page *page) {
	struct file_page *file_page UNUSED = &page->file;
	if(pml4_is_dirty(thread_current()->pml4, page->va)){	//Write back contents to file, if DIRTY.
		printf("page addr : 0x%X, is DIRTY? : %d\n", page->va, pml4_is_dirty(thread_current()->pml4, page->va));
		off_t written = file_write_at(file_page->file, page->va, file_page->read_bytes, file_page->aux.offset);
		printf("read_bytes : %d, offset : %d, WRITTEN : %d bytes.\n", file_page->read_bytes, file_page->aux.offset, written);
	}
	if(file_page->aux != NULL){	//free the LAZY_AUX.
		free(file_page->aux);
	}
	if(file_page->file != NULL){	//close the file.
		file_close(file_page->file);
	}
}

/* Do the mmap */
void *
do_mmap (void *addr, size_t length, int writable,
		struct file *file, off_t offset) {
}

/* Do the munmap */
void
do_munmap (void *addr) {
}
