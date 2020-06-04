#ifndef VM_ANON_H
#define VM_ANON_H
#include "vm/vm.h"
struct page;
enum vm_type;

#include <inttypes.h>
#include <stdint.h>

struct anon_page {
  vm_initializer* init;
  struct lazy_aux* aux;
  enum vm_type type;
  uint32_t slotNo;
};

void vm_anon_init (void);
bool anon_initializer (struct page *page, enum vm_type type, void *kva);

#endif
