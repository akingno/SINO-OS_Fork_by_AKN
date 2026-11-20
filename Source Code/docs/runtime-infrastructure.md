# Diagnostics and language resources

`src32/klog.h` declares `pr_debug`, `pr_debug_hex` and length-delimited `klog_write_bytes`. Output is always enabled, including early boot/panic, through QEMU debug port 0xE9. No prefix/newline is added. Hex output remains `0x` plus eight uppercase digits. The Ring 3 `Log` API and `SYS_LOG` ABI are unchanged; user code never writes the I/O port directly.

`resources/locale/catalog.json` is the semantic catalog; `zh-CN.json` and `en-US.json` contain translations. Keys use `MODULE_TYPE_MEANING`. Module is the owner of the message, type describes its UI purpose, and meaning must remain independent of the translated sentence. Identical text in different contexts may have distinct keys. Numeric enum values are generated implementation details, never persisted or exposed as an ABI.

`tools/build-locale.py` rejects duplicate properties/keys, missing or extra translations, inconsistent module/type prefixes and template parameters. It produces `src32/locale_keys.h` and `locale_data.inc`; edit resources, not generated files. `build32.ps1` invokes it before compiling. Development/Shell preparation generates first, then fingerprints both resources and generated inputs, preventing changed resources from bypassing build identity checks.

Use `locale_text(key)` at the UI boundary; `locale_text_for(language,key)` selects explicitly. Language 0 is Chinese and 1 English; unsupported languages fall back to English. Invalid keys return a fixed placeholder. `TextKey::NONE` returns empty text.

`locale_format` supports `{name}` (UTF-8 text) and `{count}` (u32), declared by name in catalog parameters. Pass a valid NUL-terminated name when used. Output is capacity-bounded; overflow clears the output and returns -1 rather than returning a partial UTF-8 string. Node-limit messages use the actual VFS_NODES constant. File/folder property templates preserve filenames as data.

The console outputs supplied text directly. Only cataloged messages use language keys. VFS records a typed message key, an English `vfs_error` string and a `-1` error return. Storage records diagnostics and a text key through `storage_set_error`. Repeated failures with the same cause are suppressed until recovery or a change in cause.

The default welcome/readme files remain as before. `notice` remains a modal presentation function and does not add log output. Existing `user/` applications continue to use `UiLanguage` for their own text; kernel resources are not pointers accessible to Ring 3.

`tests/host/test-src32-refactor.js` checks logging, language resources and storage. See [testing.md](testing.md) for the full test workflow.
