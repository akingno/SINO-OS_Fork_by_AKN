# Diagnostics and keyed UI text

`src32/klog.h` declares `pr_debug`, `pr_debug_hex` and length-delimited `klog_write_bytes`. Output is always enabled, including early boot/panic, through QEMU debug port 0xE9. No prefix/newline is added. Hex output remains `0x` plus eight uppercase digits. The Ring 3 `Log` API and `SYS_LOG` ABI are unchanged; user code never writes the I/O port directly.

`resources/locale/catalog.json` is the semantic catalog; `zh-CN.json` and `en-US.json` contain translations. Keys use `MODULE_TYPE_MEANING`. Module is the owner of the message, type describes its UI purpose, and meaning must remain independent of the translated sentence. Identical text in different contexts may have distinct keys. Numeric enum values are generated implementation details, never persisted or exposed as an ABI.

`tools/build-locale.py` rejects duplicate properties/keys, missing or extra translations, inconsistent module/type prefixes and template parameters. It produces `src32/locale_keys.h` and `locale_data.inc`; edit resources, not generated files. `build32.ps1` invokes it before compiling. Development/Shell preparation generates first, then fingerprints both resources and generated inputs, preventing changed resources from bypassing build identity checks.

Use `locale_text(key)` at the UI boundary; `locale_text_for(language,key)` selects explicitly. Language 0 is Chinese and 1 English; unsupported languages fall back to English. Invalid keys return a fixed placeholder. `TEXT_NONE` returns empty text. Settings and the system installer retain state as keys and resolve it when drawing. `install_set_message` and `settings_set_status` own state changes.

`locale_format` supports `{name}` (UTF-8 text) and `{count}` (u32), declared by name in catalog parameters. Pass a valid NUL-terminated name when used. Output is capacity-bounded; overflow clears the output and returns -1 rather than returning a partial UTF-8 string. Node-limit messages use the actual VFS_NODES constant. File/folder property templates preserve filenames as data.

Graphics and console functions output supplied text without translation. File contents, filenames, terminal output and the input-method dictionary are never looked up as resources. VFS records a typed message key while preserving its English `vfs_error` compatibility string and -1 return convention. GUI uses the key, not English string comparisons. Storage errors use `storage_set_error`, English diagnostics and a UI key; repeated identical failures are suppressed until recovery or a changed reason. Storage snapshot format and commit ordering are unchanged.

The default welcome/readme files remain as before. `notice` remains a modal presentation function and does not add log output. Existing `user/` applications continue to use `UiLanguage` for their own text; kernel resources are not pointers accessible to Ring 3.

Validation: run the strict infrastructure contract suite plus the system regression and both Shell suites described in testing.md. The Chinese file test waits a bounded two seconds for the actual focused window after asynchronous input; it still fails if the expected application never appears.
