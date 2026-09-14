var namespacekernel_1_1elf =
[
    [ "ElfLoader", "classkernel_1_1elf_1_1_elf_loader.html", null ],
    [ "MapOut", "structkernel_1_1elf_1_1_map_out.html", "structkernel_1_1elf_1_1_map_out" ],
    [ "LoadResult", "namespacekernel_1_1elf.html#ab90eb39dade009d486ffe40b02839796", [
      [ "OK", "namespacekernel_1_1elf.html#ab90eb39dade009d486ffe40b02839796ae0aa021e21dddbd6d8cecec71e9cf564", null ],
      [ "ALREADY_LOADING", "namespacekernel_1_1elf.html#ab90eb39dade009d486ffe40b02839796a075f821d155d361ff1eca87ebaf126e1", null ],
      [ "NOT_LOADING", "namespacekernel_1_1elf.html#ab90eb39dade009d486ffe40b02839796a42769f5c0b640211e86c29a70b3ad295", null ],
      [ "FILE_NOT_FOUND", "namespacekernel_1_1elf.html#ab90eb39dade009d486ffe40b02839796acd54d99c8efb3c2db794197045f5b83c", null ]
    ] ],
    [ "LoadState", "namespacekernel_1_1elf.html#a8f59bba1cd5bc9a93b5bd73e7c43bce5", [
      [ "IDLE", "namespacekernel_1_1elf.html#a8f59bba1cd5bc9a93b5bd73e7c43bce5aa5daf7f2ebbba4975d61dab1c40188c7", null ],
      [ "VALIDATING", "namespacekernel_1_1elf.html#a8f59bba1cd5bc9a93b5bd73e7c43bce5aff9726ecf722ce2a076e7cf705a073f7", null ],
      [ "COPYING_SEGMENTS", "namespacekernel_1_1elf.html#a8f59bba1cd5bc9a93b5bd73e7c43bce5a712317846d9b1ee3d1def3c7be3905e9", null ],
      [ "LOADING_DEPS", "namespacekernel_1_1elf.html#a8f59bba1cd5bc9a93b5bd73e7c43bce5ac7a989271cecd011fcc6b30615bebcbe", null ],
      [ "LOADING_RELOC", "namespacekernel_1_1elf.html#a8f59bba1cd5bc9a93b5bd73e7c43bce5a54f9be01bf02b6c78b84213e312d0767", null ],
      [ "MAPPING", "namespacekernel_1_1elf.html#a8f59bba1cd5bc9a93b5bd73e7c43bce5a2fa399f11879d3347f324fe703fb9f97", null ],
      [ "DONE", "namespacekernel_1_1elf.html#a8f59bba1cd5bc9a93b5bd73e7c43bce5a2ba22e58ca17bb728d522bba36cf8350", null ],
      [ "FAILED", "namespacekernel_1_1elf.html#a8f59bba1cd5bc9a93b5bd73e7c43bce5ab9e14d9b2886bcff408b85aefa780419", null ],
      [ "CANCELED", "namespacekernel_1_1elf.html#a8f59bba1cd5bc9a93b5bd73e7c43bce5ad4539bffb6062bdcbd7e7cc1b1228926", null ]
    ] ],
    [ "PostKind", "namespacekernel_1_1elf.html#abce3bf3f65d7f7924bf7925d4bf97f6f", [
      [ "STARTED", "namespacekernel_1_1elf.html#abce3bf3f65d7f7924bf7925d4bf97f6fa17130e6c806885e23770df1519b18eb7", null ],
      [ "COMPLETED", "namespacekernel_1_1elf.html#abce3bf3f65d7f7924bf7925d4bf97f6fa8f7afecbc8fbc4cd0f50a57d1172482e", null ],
      [ "OTHER", "namespacekernel_1_1elf.html#abce3bf3f65d7f7924bf7925d4bf97f6fa03570470bad94692ce93e32700d2e1cb", null ]
    ] ],
    [ "alloc_user_stack_and_heap", "namespacekernel_1_1elf.html#af0f8dea59eb8cf69559d9199d5b78d10", null ],
    [ "apply_relocations", "namespacekernel_1_1elf.html#a30d32c118d2e72c3de96f0a5b0ac13c6", null ],
    [ "elf_loader_task_main", "namespacekernel_1_1elf.html#a1c90403a3aa9677a921069fbf64f5280", null ],
    [ "exec_into_current", "namespacekernel_1_1elf.html#a8587c966825c28717e2e90c6c754b87c", null ],
    [ "finalize_loaded_task", "namespacekernel_1_1elf.html#a4a78f2b4b292d565c901908fddd7b0a4", null ],
    [ "free_resolve_images", "namespacekernel_1_1elf.html#ad5d8f8b5d835105396ec8ed79a43e212", null ],
    [ "get_needed", "namespacekernel_1_1elf.html#ad222f0d787d28d86d6767eb23d5b36c0", null ],
    [ "get_soname", "namespacekernel_1_1elf.html#a959d164cf66fb2ad436ea6771bc18caf", null ],
    [ "load", "namespacekernel_1_1elf.html#ae81746671637a90991ec1b98cef60dc4", null ],
    [ "load_shared_object", "namespacekernel_1_1elf.html#a23a6e02374b5473f5115da5597e27e19", null ],
    [ "read_dynamic_section", "namespacekernel_1_1elf.html#aa8dc005c60ba0c7cf8eba8dbd7ec8586", null ],
    [ "release_task_libs", "namespacekernel_1_1elf.html#a6f1d5d14f51004bffabfae7b764586e1", null ],
    [ "relocate_closure", "namespacekernel_1_1elf.html#aecd3436bf70035c74beb3b35afeb66da", null ],
    [ "resolve_dependencies", "namespacekernel_1_1elf.html#afbeca3dfe561183e16bf811a2f3bb139", null ],
    [ "unmap_acquired_ro", "namespacekernel_1_1elf.html#a1c98c2e07536a5a4afbbb8c71c8be241", null ],
    [ "validate_header", "namespacekernel_1_1elf.html#a53980ddec04e380da13a0325a465a554", null ],
    [ "validate_segment", "namespacekernel_1_1elf.html#a4c7c051d91c678ffce1d1a2e2b60c4f5", null ]
];