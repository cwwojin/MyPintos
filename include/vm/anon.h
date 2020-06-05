#ifndef VM_ANON_H
#define VM_ANON_H
#include "vm/vm.h"
struct page;
enum vm_type;

#include <inttypes.h>
#include <stdint.h>
#include "devices/disk.h"
#include "threads/vaddr.h"

#define SECTORS_PER_PAGE  (PGSIZE / DISK_SECTOR_SIZE)

struct swap_slot{  //This is an entry in the swap table.
  struct list_elem elem;
  bool free;
  uint32_t slotNo;  //Slot number : This corresponds to the swap disk's sector #(slotNo * 8). 
};

struct anon_page {
  vm_initializer* init;
  struct lazy_aux* aux;
  enum vm_type type;
  struct swap_slot* slot;
};


void vm_anon_init (void);
bool anon_initializer (struct page *page, enum vm_type type, void *kva);

#endif
