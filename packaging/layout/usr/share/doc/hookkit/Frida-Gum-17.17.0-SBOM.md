# Frida Gum 17.17.0 package inventory

`me.jjolano.fmwk.hookkit.gum` installs `HKGum.dylib`, a small HookKit wrapper
(`vendor/gum/hkgum.c`) linked with Frida Gum's prebuilt iOS devkit.  It is a
separate, manual-opt-in package; the HookKit framework does not link Frida
Gum.

## Devkit inputs

| Slice | Release archive | SHA-256 |
| --- | --- | --- |
| arm64 | `frida-gum-17.17.0-ios-arm64.tar.xz` | `559f62d2df37a2717c5ef292520fc277deef81924e37a838e56363d157c35cdb` |
| arm64e | `frida-gum-17.17.0-ios-arm64e.tar.xz` | `f3c2fe3cb2db0364ffb51bd3d8250d2ce8d515dc74147a0485c44698b028d29f` |

The source release is [Frida Gum 17.17.0](https://github.com/frida/frida-gum/tree/17.17.0).
The static archive inspected for this inventory contains 650 object members.

## Linked component inventory

| Component | Source revision | License notice in this package |
| --- | --- | --- |
| Frida Gum | [17.17.0](https://github.com/frida/frida-gum/tree/17.17.0) | `licenses/Frida-Gum-wxWindows-3.1.txt` |
| Capstone | [d536b1577fd033a31d75f48fd183aa425256cc18](https://github.com/frida/capstone/tree/d536b1577fd033a31d75f48fd183aa425256cc18) | `licenses/Capstone-BSD-3-Clause.txt` |
| GLib, GObject, GIO, GModule, GVDB, XDG MIME | [790ffa82e80d99fba8a3db494e46f907d560893c](https://github.com/frida/glib/tree/790ffa82e80d99fba8a3db494e46f907d560893c) | `licenses/GLib-LGPL-2.1-or-later.txt` |
| libffi | [c06a577933f07c76c3f536c9e4ba0f7d9c1e8c4d](https://github.com/frida/libffi/tree/c06a577933f07c76c3f536c9e4ba0f7d9c1e8c4d) | `licenses/libffi-MIT.txt` |
| PCRE2 | [b47486922fdc3486499b310dc9cf903449700474](https://github.com/frida/pcre2/tree/b47486922fdc3486499b310dc9cf903449700474) | `licenses/PCRE2-BSD-3-Clause.txt` |
| zlib | [171a3eacaea8b731ef1fc586e7777b77742e2a1d](https://github.com/frida/zlib/tree/171a3eacaea8b731ef1fc586e7777b77742e2a1d) | `licenses/zlib.txt` |
| libiconv / libcharset | [bbbf4561da4847bf95ce9458da76e072b77cabd1](https://github.com/frida/libiconv/tree/bbbf4561da4847bf95ce9458da76e072b77cabd1) | `licenses/GLib-LGPL-2.1-or-later.txt` |
| GLib proxy-libintl objects | Frida GLib stack above | `licenses/Frida-Gum-wxWindows-3.1.txt` |
| XZ / liblzma | [e70f5800ab5001c9509d374dbf3e7e6b866c43fe](https://github.com/frida/xz/tree/e70f5800ab5001c9509d374dbf3e7e6b866c43fe) | `licenses/XZ-Utils.txt` |

The inventory was derived from the object-prefixes in the distributed
`libfrida-gum.a`; build-only tooling and source-only vendored material are
not claimed as shipped library contents.  This factual inventory does not
express a legal compatibility determination.
