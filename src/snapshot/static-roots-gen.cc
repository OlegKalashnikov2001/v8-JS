// Copyright 2018 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/snapshot/static-roots-gen.h"

#include <fstream>

#include "src/common/globals.h"
#include "src/common/ptr-compr-inl.h"
#include "src/execution/isolate.h"
#include "src/objects/instance-type-inl.h"
#include "src/objects/instance-type.h"
#include "src/objects/objects-definitions.h"
#include "src/objects/visitors.h"
#include "src/roots/roots-inl.h"
#include "src/roots/roots.h"

namespace v8 {
namespace internal {

class StaticRootsTableGenImpl {
 public:
  explicit StaticRootsTableGenImpl(Isolate* isolate) {
    // Collect all roots
    ReadOnlyRoots ro_roots(isolate);
    {
      RootIndex pos = RootIndex::kFirstReadOnlyRoot;
#define ADD_ROOT(_, value, CamelName)                       \
  {                                                         \
    Tagged_t ptr = V8HeapCompressionScheme::CompressTagged( \
        ro_roots.unchecked_##value().ptr());                \
    sorted_roots_[ptr].push_back(pos);                      \
    camel_names_[RootIndex::k##CamelName] = #CamelName;     \
    ++pos;                                                  \
  }
      READ_ONLY_ROOT_LIST(ADD_ROOT)
#undef ADD_ROOT
    }
  }

  const std::map<Tagged_t, std::list<RootIndex>>& sorted_roots() {
    return sorted_roots_;
  }

  const std::string& camel_name(RootIndex idx) { return camel_names_.at(idx); }

 private:
  std::map<Tagged_t, std::list<RootIndex>> sorted_roots_;
  std::unordered_map<RootIndex, std::string> camel_names_;
};

void StaticRootsTableGen::write(Isolate* isolate, const char* file) {
  CHECK_WITH_MSG(!V8_STATIC_ROOTS_BOOL,
                 "Re-generating the table of roots is only supported in builds "
                 "with v8_enable_static_roots disabled");
  CHECK(file);
  static_assert(static_cast<int>(RootIndex::kFirstReadOnlyRoot) == 0);

  std::ofstream out(file, std::ios::binary);

  out << "// Copyright 2022 the V8 project authors. All rights reserved.\n"
      << "// Use of this source code is governed by a BSD-style license "
         "that can be\n"
      << "// found in the LICENSE file.\n"
      << "\n"
      << "// This file is automatically generated by "
         "`tools/dev/gen-static-roots.py`. Do\n// not edit manually.\n"
      << "\n"
      << "#ifndef V8_ROOTS_STATIC_ROOTS_H_\n"
      << "#define V8_ROOTS_STATIC_ROOTS_H_\n"
      << "\n"
      << "#include \"src/common/globals.h\"\n"
      << "\n"
      << "#if V8_STATIC_ROOTS_BOOL\n"
      << "\n"
      << "#include \"src/objects/instance-type.h\"\n"
      << "#include \"src/roots/roots.h\"\n"
      << "\n"
      << "// Disabling Wasm or Intl invalidates the contents of "
         "static-roots.h.\n"
      << "// TODO(olivf): To support static roots for multiple build "
         "configurations we\n"
      << "//              will need to generate target specific versions of "
         "this file.\n"
      << "static_assert(V8_ENABLE_WEBASSEMBLY);\n"
      << "static_assert(V8_INTL_SUPPORT);\n"
      << "\n"
      << "namespace v8 {\n"
      << "namespace internal {\n"
      << "\n"
      << "struct StaticReadOnlyRoot {\n";

  // Output a symbol for every root. Ordered by ptr to make it easier to see the
  // memory layout of the read only page.
  const auto size = static_cast<int>(RootIndex::kReadOnlyRootsCount);
  StaticRootsTableGenImpl gen(isolate);

  for (auto& entry : gen.sorted_roots()) {
    Tagged_t ptr = entry.first;
    const std::list<RootIndex>& roots = entry.second;

    for (RootIndex root : roots) {
      static const char* kPreString = "  static constexpr Tagged_t k";
      const std::string& name = gen.camel_name(root);
      size_t ptr_len = ceil(log2(ptr) / 4.0);
      // Full line is: "kPreString|name = 0x.....;"
      size_t len = strlen(kPreString) + name.length() + 5 + ptr_len + 1;
      out << kPreString << name << " =";
      if (len > 80) out << "\n     ";
      out << " 0x" << std::hex << ptr << std::dec << ";\n";
    }
  }
  out << "};\n";

  // Output in order of roots table
  out << "\nstatic constexpr std::array<Tagged_t, " << size
      << "> StaticReadOnlyRootsPointerTable = {\n";

  {
#define ENTRY(_1, _2, CamelName) \
  out << "    StaticReadOnlyRoot::k" << #CamelName << ",\n";
    READ_ONLY_ROOT_LIST(ENTRY)
#undef ENTRY
    out << "};\n";
  }
  out << "\n"
      << "}  // namespace internal\n"
      << "}  // namespace v8\n"
      << "#endif  // V8_STATIC_ROOTS_BOOL\n"
      << "#endif  // V8_ROOTS_STATIC_ROOTS_H_\n";
}

}  // namespace internal
}  // namespace v8
