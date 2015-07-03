/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2013 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/objects/xuser_module.h"

#include "xenia/base/logging.h"
#include "xenia/cpu/processor.h"
#include "xenia/cpu/xex_module.h"
#include "xenia/emulator.h"
#include "xenia/kernel/objects/xfile.h"
#include "xenia/kernel/objects/xthread.h"

namespace xe {
namespace kernel {

using namespace xe::cpu;

XUserModule::XUserModule(KernelState* kernel_state, const char* path)
    : XModule(kernel_state, ModuleType::kUserModule, path) {}

XUserModule::~XUserModule() {}

X_STATUS XUserModule::LoadFromFile(std::string path) {
  X_STATUS result = X_STATUS_UNSUCCESSFUL;

  // Resolve the file to open.
  // TODO(benvanik): make this code shared?
  auto fs_entry = kernel_state()->file_system()->ResolvePath(path);
  if (!fs_entry) {
    XELOGE("File not found: %s", path.c_str());
    return X_STATUS_NO_SUCH_FILE;
  }

  // If the FS supports mapping, map the file in and load from that.
  if (fs_entry->can_map()) {
    // Map.
    auto mmap = fs_entry->OpenMapped(MappedMemory::Mode::kRead);
    if (!mmap) {
      return result;
    }

    // Load the module.
    result = LoadFromMemory(mmap->data(), mmap->size());
  } else {
    std::vector<uint8_t> buffer(fs_entry->size());

    // Open file for reading.
    object_ref<XFile> file;
    result =
        fs_entry->Open(kernel_state(), vfs::FileAccess::kGenericRead, &file);
    if (result) {
      return result;
    }

    // Read entire file into memory.
    // Ugh.
    size_t bytes_read = 0;
    result = file->Read(buffer.data(), buffer.size(), 0, &bytes_read);
    if (result) {
      return result;
    }

    // Load the module.
    result = LoadFromMemory(buffer.data(), bytes_read);
  }

  return result;
}

X_STATUS XUserModule::LoadFromMemory(const void* addr, const size_t length) {
  Processor* processor = kernel_state()->processor();

  // Prepare the module for execution.
  // Runtime takes ownership.
  auto xex_module = std::make_unique<XexModule>(processor, kernel_state());
  if (!xex_module->Load(name_, path_, addr, length)) {
    return X_STATUS_UNSUCCESSFUL;
  }
  processor_module_ = xex_module.get();
  if (!processor->AddModule(std::move(xex_module))) {
    return X_STATUS_UNSUCCESSFUL;
  }

  // Copy the xex2 header into guest memory.
  const xex2_header* header = this->xex_module()->xex_header();
  guest_xex_header_ = memory()->SystemHeapAlloc(header->header_size);

  uint8_t* xex_header_ptr = memory()->TranslateVirtual(guest_xex_header_);
  std::memcpy(xex_header_ptr, header, header->header_size);

  // Setup the loader data entry
  auto ldr_data =
      memory()->TranslateVirtual<X_LDR_DATA_TABLE_ENTRY*>(hmodule_ptr_);

  ldr_data->dll_base = 0;  // GetProcAddress will read this.
  ldr_data->xex_header_base = guest_xex_header_;

  // Cache some commonly used headers...
  this->xex_module()->GetOptHeader(XEX_HEADER_ENTRY_POINT,
                                   &entry_point_);
  this->xex_module()->GetOptHeader(XEX_HEADER_DEFAULT_STACK_SIZE,
                                   &stack_size_);

  OnLoad();

  return X_STATUS_SUCCESS;
}

uint32_t XUserModule::GetProcAddressByOrdinal(uint16_t ordinal) {
  return xex_module()->GetProcAddress(ordinal);
}

uint32_t XUserModule::GetProcAddressByName(const char* name) {
  return xex_module()->GetProcAddress(name);
}

X_STATUS XUserModule::GetSection(const char* name, uint32_t* out_section_data,
                                 uint32_t* out_section_size) {
  xex2_opt_resource_info* resource_header = nullptr;
  if (!XexModule::GetOptHeader(xex_header(), XEX_HEADER_RESOURCE_INFO,
                               &resource_header)) {
    // No resources.
    return X_STATUS_UNSUCCESSFUL;
  }

  uint32_t count = (resource_header->size - 4) / 16;
  for (uint32_t i = 0; i < count; i++) {
    auto& res = resource_header->resources[i];
    if (strcmp(name, res.name) == 0) {
      // Found!
      *out_section_data = res.address;
      *out_section_size = res.size;

      return X_STATUS_SUCCESS;
    }
  }

  return X_STATUS_UNSUCCESSFUL;
}

X_STATUS XUserModule::GetOptHeader(xe_xex2_header_keys key, void** out_ptr) {
  assert_not_null(out_ptr);

  bool ret = xex_module()->GetOptHeader(key, out_ptr);
  if (!ret) {
    return X_STATUS_NOT_FOUND;
  }

  return X_STATUS_SUCCESS;
}

X_STATUS XUserModule::GetOptHeader(xe_xex2_header_keys key,
                                   uint32_t* out_header_guest_ptr) {
  auto header = xex_module()->xex_header();
  if (!header) {
    return X_STATUS_UNSUCCESSFUL;
  }
  return GetOptHeader(memory()->virtual_membase(), header, key,
                      out_header_guest_ptr);
}

X_STATUS XUserModule::GetOptHeader(uint8_t* membase, const xex2_header* header,
                                   xe_xex2_header_keys key,
                                   uint32_t* out_header_guest_ptr) {
  assert_not_null(out_header_guest_ptr);
  uint32_t field_value = 0;
  bool field_found = false;
  for (uint32_t i = 0; i < header->header_count; i++) {
    auto& opt_header = header->headers[i];
    if (opt_header.key != key) {
      continue;
    }
    field_found = true;
    switch (opt_header.key & 0xFF) {
      case 0x00:
        // Return data stored in header value.
        field_value = opt_header.value;
        break;
      case 0x01:
        // Return pointer to data stored in header value.
        field_value = uint32_t((uint8_t*)&opt_header.value - membase);
        break;
      default:
        // Data stored at offset to header.
        field_value = uint32_t((uint8_t*)header - membase) + opt_header.offset;
        break;
    }
    break;
  }
  *out_header_guest_ptr = field_value;
  if (!field_found) {
    return X_STATUS_NOT_FOUND;
  }
  return X_STATUS_SUCCESS;
}

X_STATUS XUserModule::Launch(uint32_t flags) {
  XELOGI("Launching module...");
  Dump();

  // Create a thread to run in.
  auto thread = object_ref<XThread>(
      new XThread(kernel_state(), stack_size_, 0, entry_point_, 0, 0));

  X_STATUS result = thread->Create();
  if (XFAILED(result)) {
    XELOGE("Could not create launch thread: %.8X", result);
    return result;
  }

  // Wait until thread completes.
  thread->Wait(0, 0, 0, nullptr);

  return X_STATUS_SUCCESS;
}

void XUserModule::Dump() {
  xe::cpu::ExportResolver* export_resolver =
      kernel_state_->emulator()->export_resolver();
  auto header = xex_header();

  // TODO: Need to loop through the optional headers one-by-one.

  // XEX header.
  printf("Module %s:\n", path_.c_str());
  printf("    Module Flags: %.8X\n", (uint32_t)header->module_flags);

  // Security header
  auto security_info = xex_module()->xex_security_info();
  printf("Security Header:\n");
  printf("     Image Flags: %.8X\n", (uint32_t)security_info->image_flags);
  printf("    Load Address: %.8X\n", (uint32_t)security_info->load_address);
  printf("      Image Size: %.8X\n", (uint32_t)security_info->image_size);
  printf("    Export Table: %.8X\n", (uint32_t)security_info->export_table);

  // Optional headers
  printf("Optional Header Count: %d\n", (uint32_t)header->header_count);

  for (uint32_t i = 0; i < header->header_count; i++) {
    auto& opt_header = header->headers[i];

    // Stash a pointer (although this isn't used in every case)
    void* opt_header_ptr = (uint8_t*)header + opt_header.offset;
    switch (opt_header.key) {
      case XEX_HEADER_RESOURCE_INFO: {
        printf("  XEX_HEADER_RESOURCE_INFO (TODO):\n");
        auto opt_resource_info =
            reinterpret_cast<xex2_opt_resource_info*>(opt_header_ptr);

      } break;
      case XEX_HEADER_FILE_FORMAT_INFO: {
        printf("  XEX_HEADER_FILE_FORMAT_INFO (TODO):\n");
      } break;
      case XEX_HEADER_DELTA_PATCH_DESCRIPTOR: {
        printf("  XEX_HEADER_DELTA_PATCH_DESCRIPTOR (TODO):\n");
      } break;
      case XEX_HEADER_BOUNDING_PATH: {
        auto opt_bound_path =
            reinterpret_cast<xex2_opt_bound_path*>(opt_header_ptr);
        printf("  XEX_HEADER_BOUNDING_PATH: %s\n", opt_bound_path->path);
      } break;
      case XEX_HEADER_ORIGINAL_BASE_ADDRESS: {
        printf("  XEX_HEADER_ORIGINAL_BASE_ADDRESS: %.8X\n", (uint32_t)opt_header.value);
      } break;
      case XEX_HEADER_ENTRY_POINT: {
        printf("  XEX_HEADER_ENTRY_POINT: %.8X\n", (uint32_t)opt_header.value);
      } break;
      case XEX_HEADER_IMAGE_BASE_ADDRESS: {
        printf("  XEX_HEADER_IMAGE_BASE_ADDRESS: %.8X\n", (uint32_t)opt_header.value);
      } break;
      case XEX_HEADER_ORIGINAL_PE_NAME: {
        auto opt_pe_name =
            reinterpret_cast<xex2_opt_original_pe_name*>(opt_header_ptr);
        printf("  XEX_HEADER_ORIGINAL_PE_NAME: %s\n", opt_pe_name->name);
      } break;
      case XEX_HEADER_STATIC_LIBRARIES: {
        printf("  XEX_HEADER_STATIC_LIBRARIES:\n");
        auto opt_static_libraries =
            reinterpret_cast<const xex2_opt_static_libraries*>(opt_header_ptr);

        uint32_t count = (opt_static_libraries->size - 4) / 0x10;
        for (uint32_t i = 0; i < count; i++) {
          auto& library = opt_static_libraries->libraries[i];
          printf(
              "    %-8s : %d.%d.%d.%d\n", library.name,
              (uint16_t)library.version_major, (uint16_t)library.version_minor,
              (uint16_t)library.version_build, (uint16_t)library.version_qfe);
        }
      } break;
      case XEX_HEADER_TLS_INFO: {
        printf("  XEX_HEADER_TLS_INFO:\n");
        auto opt_tls_info =
            reinterpret_cast<const xex2_opt_tls_info*>(opt_header_ptr);

        printf("          Slot Count: %d\n", (uint32_t)opt_tls_info->slot_count);
        printf("    Raw Data Address: %.8X\n", (uint32_t)opt_tls_info->raw_data_address);
        printf("           Data Size: %d\n", (uint32_t)opt_tls_info->data_size);
        printf("       Raw Data Size: %d\n", (uint32_t)opt_tls_info->raw_data_size);
      } break;
      case XEX_HEADER_DEFAULT_STACK_SIZE: {
        printf("  XEX_HEADER_DEFAULT_STACK_SIZE: %d\n", (uint32_t)opt_header.value);
      } break;
      case XEX_HEADER_DEFAULT_FILESYSTEM_CACHE_SIZE: {
        printf("  XEX_HEADER_DEFAULT_FILESYSTEM_CACHE_SIZE: %d\n", (uint32_t)opt_header.value);
      } break;
      case XEX_HEADER_DEFAULT_HEAP_SIZE: {
        printf("  XEX_HEADER_DEFAULT_HEAP_SIZE: %d\n", (uint32_t)opt_header.value);
      } break;
      case XEX_HEADER_PAGE_HEAP_SIZE_AND_FLAGS: {
        printf("  XEX_HEADER_PAGE_HEAP_SIZE_AND_FLAGS (TODO):\n");
      } break;
      case XEX_HEADER_SYSTEM_FLAGS: {
        printf("  XEX_HEADER_SYSTEM_FLAGS (TODO):\n");
      } break;
      case XEX_HEADER_EXECUTION_INFO: {
        printf("  XEX_HEADER_EXECUTION_INFO (TODO):\n");
      } break;
      case XEX_HEADER_TITLE_WORKSPACE_SIZE: {
        printf("  XEX_HEADER_TITLE_WORKSPACE_SIZE (TODO):\n");
      } break;
      case XEX_HEADER_GAME_RATINGS: {
        printf("  XEX_HEADER_GAME_RATINGS (TODO):\n");
      } break;
      case XEX_HEADER_LAN_KEY: {
        printf("  XEX_HEADER_LAN_KEY (TODO):\n");
      } break;
      case XEX_HEADER_XBOX360_LOGO: {
        printf("  XEX_HEADER_XBOX360_LOGO (TODO):\n");
      } break;
      case XEX_HEADER_MULTIDISC_MEDIA_IDS: {
        printf("  XEX_HEADER_MULTIDISC_MEDIA_IDS (TODO):\n");
      } break;
      case XEX_HEADER_ALTERNATE_TITLE_IDS: {
        printf("  XEX_HEADER_ALTERNATE_TITLE_IDS (TODO):\n");
      } break;
      case XEX_HEADER_ADDITIONAL_TITLE_MEMORY: {
        printf("  XEX_HEADER_ADDITIONAL_TITLE_MEMORY (TODO):\n");
      } break;
      case XEX_HEADER_EXPORTS_BY_NAME: {
        printf("  XEX_HEADER_EXPORTS_BY_NAME (TODO):\n");
      } break;
    }
  }

  /*
  printf("    System Flags: %.8X\n", header->system_flags);
  printf("\n");
  printf("         Address: %.8X\n", header->exe_address);
  printf("     Entry Point: %.8X\n", header->exe_entry_point);
  printf("      Stack Size: %.8X\n", header->exe_stack_size);
  printf("       Heap Size: %.8X\n", header->exe_heap_size);
  printf("\n");
  printf("  Execution Info:\n");
  printf("        Media ID: %.8X\n", header->execution_info.media_id);
  printf(
      "         Version: %d.%d.%d.%d\n", header->execution_info.version.major,
      header->execution_info.version.minor,
      header->execution_info.version.build, header->execution_info.version.qfe);
  printf("    Base Version: %d.%d.%d.%d\n",
         header->execution_info.base_version.major,
         header->execution_info.base_version.minor,
         header->execution_info.base_version.build,
         header->execution_info.base_version.qfe);
  printf("        Title ID: %.8X\n", header->execution_info.title_id);
  printf("        Platform: %.8X\n", header->execution_info.platform);
  printf("      Exec Table: %.8X\n", header->execution_info.executable_table);
  printf("     Disc Number: %d\n", header->execution_info.disc_number);
  printf("      Disc Count: %d\n", header->execution_info.disc_count);
  printf("     Savegame ID: %.8X\n", header->execution_info.savegame_id);
  printf("\n");
  printf("  Loader Info:\n");
  printf("     Image Flags: %.8X\n", header->loader_info.image_flags);
  printf("    Game Regions: %.8X\n", header->loader_info.game_regions);
  printf("     Media Flags: %.8X\n", header->loader_info.media_flags);
  printf("\n");
  printf("  TLS Info:\n");
  printf("      Slot Count: %d\n", header->tls_info.slot_count);
  printf("       Data Size: %db\n", header->tls_info.data_size);
  printf("         Address: %.8X, %db\n", header->tls_info.raw_data_address,
         header->tls_info.raw_data_size);
  printf("\n");
  printf("  Headers:\n");
  for (size_t n = 0; n < header->header_count; n++) {
    const xex2_opt_header* opt_header = &header->headers[n];
    printf("    %.8X (%.8X, %4db) %.8X = %11d\n", opt_header->key,
           opt_header->offset, opt_header->length, opt_header->value,
           opt_header->value);
  }
  printf("\n");

  // Resources.
  printf("Resources:\n");
  for (size_t n = 0; n < header->resource_info_count; n++) {
    auto& res = header->resource_infos[n];
    printf("  %-8s %.8X-%.8X, %db\n", res.name, res.address,
           res.address + res.size, res.size);
  }
  printf("\n");

  // Section info.
  printf("Sections:\n");
  for (size_t n = 0, i = 0; n < header->section_count; n++) {
    const xe_xex2_section_t* section = &header->sections[n];
    const char* type = "UNKNOWN";
    switch (section->info.type) {
      case XEX_SECTION_CODE:
        type = "CODE   ";
        break;
      case XEX_SECTION_DATA:
        type = "RWDATA ";
        break;
      case XEX_SECTION_READONLY_DATA:
        type = "RODATA ";
        break;
    }
    const size_t start_address = header->exe_address + (i * section->page_size);
    const size_t end_address =
        start_address + (section->info.page_count * section->page_size);
    printf("  %3d %s %3d pages    %.8X - %.8X (%d bytes)\n", (int)n, type,
           section->info.page_count, (int)start_address, (int)end_address,
           section->info.page_count * section->page_size);
    i += section->info.page_count;
  }
  printf("\n");

  // Static libraries.
  printf("Static Libraries:\n");
  for (size_t n = 0; n < header->static_library_count; n++) {
    const xe_xex2_static_library_t* library = &header->static_libraries[n];
    printf("  %-8s : %d.%d.%d.%d\n", library->name, library->major,
           library->minor, library->build, library->qfe);
  }
  printf("\n");

  // Exports.
  printf("Exports:\n");
  if (header->loader_info.export_table) {
    printf(" XEX-style Ordinal Exports:\n");
    auto export_table = reinterpret_cast<const xe_xex2_export_table*>(
        memory()->TranslateVirtual(header->loader_info.export_table));
    uint32_t ordinal_count = xe::byte_swap(export_table->count);
    uint32_t ordinal_base = xe::byte_swap(export_table->base);
    for (uint32_t i = 0, ordinal = ordinal_base; i < ordinal_count;
         ++i, ++ordinal) {
      uint32_t ordinal_offset = xe::byte_swap(export_table->ordOffset[i]);
      ordinal_offset += xe::byte_swap(export_table->imagebaseaddr) << 16;
      printf("     %.8X          %.3X (%3d)\n", ordinal_offset, ordinal,
             ordinal);
    }
  }
  if (header->pe_export_table_offset) {
    auto e = reinterpret_cast<const IMAGE_EXPORT_DIRECTORY*>(
        memory()->TranslateVirtual(header->exe_address +
                                   header->pe_export_table_offset));
    uint32_t* function_table = (uint32_t*)((uint64_t)e + e->AddressOfFunctions);
    uint32_t* name_table = (uint32_t*)((uint64_t)e + e->AddressOfNames);
    uint16_t* ordinal_table =
        (uint16_t*)((uint64_t)e + e->AddressOfNameOrdinals);
    const char* mod_name = (const char*)((uint64_t)e + e->Name);
    printf(" PE %s:\n", mod_name);
    for (uint32_t i = 0; i < e->NumberOfNames; i++) {
      const char* fn_name = (const char*)((uint64_t)e + name_table[i]);
      uint16_t ordinal = ordinal_table[i];
      uint32_t addr = header->exe_address + function_table[ordinal];
      printf("     %.8X          %.3X (%3d)    %s\n", addr, ordinal, ordinal,
             fn_name);
    }
  }
  if (!header->loader_info.export_table && !header->pe_export_table_offset) {
    printf("  No exports\n");
  }
  printf("\n");

  // Imports.
  printf("Imports:\n");
  for (size_t n = 0; n < header->import_library_count; n++) {
    const xe_xex2_import_library_t* library = &header->import_libraries[n];

    xe_xex2_import_info_t* import_infos;
    size_t import_info_count;
    if (!xe_xex2_get_import_infos(xex_, library, &import_infos,
                                  &import_info_count)) {
      printf(" %s - %d imports\n", library->name, (int)import_info_count);
      printf("   Version: %d.%d.%d.%d\n", library->version.major,
             library->version.minor, library->version.build,
             library->version.qfe);
      printf("   Min Version: %d.%d.%d.%d\n", library->min_version.major,
             library->min_version.minor, library->min_version.build,
             library->min_version.qfe);
      printf("\n");

      // Counts.
      int known_count = 0;
      int unknown_count = 0;
      int impl_count = 0;
      int unimpl_count = 0;
      for (size_t m = 0; m < import_info_count; m++) {
        const xe_xex2_import_info_t* info = &import_infos[m];

        if (kernel_state_->IsKernelModule(library->name)) {
          auto kernel_export =
              export_resolver->GetExportByOrdinal(library->name, info->ordinal);
          if (kernel_export) {
            known_count++;
            if (kernel_export->is_implemented()) {
              impl_count++;
            } else {
              unimpl_count++;
            }
          } else {
            unknown_count++;
            unimpl_count++;
          }
        } else {
          auto module = kernel_state_->GetModule(library->name);
          if (module) {
            uint32_t export_addr =
                module->GetProcAddressByOrdinal(info->ordinal);
            if (export_addr) {
              impl_count++;
              known_count++;
            } else {
              unimpl_count++;
              unknown_count++;
            }
          } else {
            unimpl_count++;
            unknown_count++;
          }
        }
      }
      printf("         Total: %4u\n", uint32_t(import_info_count));
      printf("         Known:  %3d%% (%d known, %d unknown)\n",
             (int)(known_count / (float)import_info_count * 100.0f),
             known_count, unknown_count);
      printf("   Implemented:  %3d%% (%d implemented, %d unimplemented)\n",
             (int)(impl_count / (float)import_info_count * 100.0f), impl_count,
             unimpl_count);
      printf("\n");

      // Listing.
      for (size_t m = 0; m < import_info_count; m++) {
        const xe_xex2_import_info_t* info = &import_infos[m];
        const char* name = "UNKNOWN";
        bool implemented = false;

        Export* kernel_export = nullptr;
        if (kernel_state_->IsKernelModule(library->name)) {
          kernel_export =
              export_resolver->GetExportByOrdinal(library->name, info->ordinal);
          if (kernel_export) {
            name = kernel_export->name;
            implemented = kernel_export->is_implemented();
          }
        } else {
          auto module = kernel_state_->GetModule(library->name);
          if (module && module->GetProcAddressByOrdinal(info->ordinal)) {
            // TODO: Name lookup
            implemented = true;
          }
        }
        if (kernel_export && kernel_export->type == Export::Type::kVariable) {
          printf("   V %.8X          %.3X (%3d) %s %s\n", info->value_address,
                 info->ordinal, info->ordinal, implemented ? "  " : "!!", name);
        } else if (info->thunk_address) {
          printf("   F %.8X %.8X %.3X (%3d) %s %s\n", info->value_address,
                 info->thunk_address, info->ordinal, info->ordinal,
                 implemented ? "  " : "!!", name);
        }
      }
    }

    printf("\n");
  }
  */
}

}  // namespace kernel
}  // namespace xe
