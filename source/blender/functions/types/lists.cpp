#include "BLI_lazy_init.hpp"

#include "FN_cpp.hpp"
#include "FN_llvm.hpp"
#include "FN_types.hpp"

namespace FN {
namespace Types {

class ListTypeInfo : public CPPTypeInfoForType<SharedList> {
 private:
  Type *m_base_type;

 public:
  ListTypeInfo(Type *base_type) : m_base_type(std::move(base_type))
  {
  }

  void construct_default(void *ptr) const override
  {
    new (ptr) SharedList(new List(m_base_type));
  }

  void construct_default_n(void *ptr, uint n) const override
  {
    SharedList *ptr_ = static_cast<SharedList *>(ptr);
    for (uint i = 0; i < n; i++) {
      new (ptr_ + i) SharedList(new List(m_base_type));
    }
  }
};

Type *new_list_type(Type *base_type)
{
  Type *type = new Type(base_type->name() + " List");
  type->add_extension<ListTypeInfo>(base_type);
  type->add_extension<PointerLLVMTypeInfo>(
      /* Copy list by incrementing the reference counter. */
      [](void *list) -> void * {
        List *list_ = static_cast<List *>(list);
        list_->incref();
        return static_cast<void *>(list);
      },
      /* Free list by decrementing the reference counter. */
      [](void *list) {
        List *list_ = static_cast<List *>(list);
        list_->decref();
      },
      /* Create a new empty list. */
      [base_type]() -> void * {
        List *list = new List(base_type);
        return static_cast<void *>(list);
      });
  return type;
}

}  // namespace Types
}  // namespace FN