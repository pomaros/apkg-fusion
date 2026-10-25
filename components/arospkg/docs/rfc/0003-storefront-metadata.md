---
title: "RFC 0003: storefront metadata (pictures and offers)"
status: proposal
created: 2026-10-12
---

# RFC 0003: storefront metadata — pictures and offers

| | |
|---|---|
| **Status** | **Proposal.** Nothing in the public catalogue carries these fields, no client reads them from a catalogue, and `tools/mkindex.py` does not accept them. |
| **What exists** | A **demonstration** in PkgManager 0.5.0: a local file shipped beside PkgManager (`Demo/storefront.json`) with pictures for a few real applications and one fictional paid application. Section 5. |
| **Companion** | [RFC 0002](0002-application-folder.md), the application folder |

## 1. Why this is only a proposal

Two things a package manager for AROS might show one day — pictures of an
application, and an application that costs money — raise questions this
project has not answered: who hosts pictures and how a client fetches them
without blocking its window; who sells, who keeps the money, what a
licence is, and what a buyer can rely on. The demonstration shows the
direction in the window so those questions can be discussed with
something to look at. It does not answer them.

## 2. Pictures: proposed fields

<!-- proposal -->
```toml
[[media]]                       # proposed; not accepted by the catalogue
type    = "screenshot"          # "icon", "screenshot"
url     = "https://example.org/folio-1.png"
sha256  = "…"                   # checked like an archive
size    = 61234                 # bytes
width   = 640                   # pixels, as the file says
height  = 400
caption = "Two-page spread with search results"
```

| field | proposal |
|---|---|
| `type` | `icon` (one, at most 64 x 64) or `screenshot` (up to 6). |
| `url` | HTTPS, like archive URLs; a stable address. |
| `sha256`, `size` | Required, checked before a picture is shown, as an archive is checked before it is unpacked. |
| `width`, `height` | Required; a client refuses a file whose header says otherwise. Screenshots at most 1280 x 800. |
| `caption` | Plain ASCII, at most 120 characters, as `post_install_notes` lines are. |
| format | PNG only, at most 512 KiB, so every AROS with a PNG datatype shows it. |

Rules a client would follow:

- Pictures are **never required**: a package without them installs and
  shows text, as now.
- Pictures are **fetched off the window's task**, into the cache, with
  the same size and hash checks as archives, and never block a button.
- A picture that fails a check is **not shown**, and the panel says why in
  a line of text; it never falls back to an unchecked file.
- Pictures of a program are **real screenshots of that program**; a
  drawing is marked as a mockup in its caption and in the picture itself.

## 3. Offers: proposed fields

<!-- proposal -->
```toml
[offer]                         # proposed; not accepted by the catalogue
price    = "9.99"               # a decimal string, never computed with
currency = "EUR"                # ISO 4217
seller   = "Example Software"
terms    = "https://example.org/licence"
purchase = "https://example.org/buy/paintstudio"
```

What a client would do with them, as a proposal:

- Show the price, the seller and the terms beside the package.
- **Buy** opens the seller's own page (`purchase`) in the user's browser.
  The client never asks for card or account details and never sees them.
- After buying, the seller's licence or download is the seller's
  business; the client installs a package as it installs any other.

What this proposal deliberately leaves out: accounts, licence keys,
copy protection, refunds, taxes, and any payment service. Each of those
needs its own design and its own agreement with the people involved.

## 4. Where the fields would live

In the catalogue manifest beside `[launch]`, carried into `index-v2.json`
only. `index.json`, for clients up to 0.3.2, would never carry them.

## 5. The demonstration in PkgManager 0.5.0

- **File**: `Demo/storefront.json` beside PkgManager, read once at
  startup. It is built by `tools/make-demo-store.py` from
  `docs/demo-store/sources.json`. PkgManager runs the same without it.
- **Pictures**: for Folio and GrafX2, real screenshots taken from those
  programs on AROS One 1.3 under QEMU on 2026-10-12, and their own icons
  copied from their release archives. Each set says where it comes from
  under the pictures. Pictures sit in fixed places: 320 x 200 for the
  screenshot, 80 x 50 for thumbnails, 64 x 64 for the icon. A missing or
  oversized picture shows as a line of text. Packages without pictures
  show text only.
- **Offer**: one fictional application, *Example Paint Studio*, 9.99 EUR,
  seller *Example Software (fictional)*, with a picture marked
  `MOCKUP`. It is listed only under the *Demo store* filter, its state
  column says *demo offer*, and its details start with
  "Demo -- purchasing is not available yet".
- **Buy...** opens a window that explains how a purchase could work
  (section 3) and has only a Close button. Nothing is bought, installed
  or sent; no data is asked for.
- The demonstration's offer ids start with `demo-`, so they cannot be
  mistaken for a package; the catalogue does not contain them.
