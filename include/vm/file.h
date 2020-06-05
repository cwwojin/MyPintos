#ifndef VM_FILE_H
#define VM_FILE_H
#include "filesys/file.h"
#include "vm/vm.h"

struct page;
enum vm_type;

struct file_page {
	vm_initializer *init;
	struct lazy_aux* aux;
	enum vm_type type;
	struct file* file;
	size_t read_bytes;
	bool next_page;		//is the NEXT page (addr + PGSIZE) also a file_page mapped to the same file?
	bool swapped_out;	//was this page swapped-out before?
};

void vm_file_init (void);
bool file_map_initializer (struct page *page, enum vm_type type, void *kva);
void *do_mmap(void *addr, size_t length, int writable,
		struct file *file, off_t offset);
void do_munmap (void *va);
#endif
