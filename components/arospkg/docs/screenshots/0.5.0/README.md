# arospkg 0.5.0 screenshots

Screenshots from arospkg 0.5.0 on AROS One 1.3, x86_64 ABIv11,
1024x768. Terminal examples use Limpet 0.3.0.

What is real and what is a demonstration:

- Folio, GrafX2 and grep are real packages from the catalogue (their
  archives as published), installed by apkg into a test root `RAM:PG`.
- **folio 0.4.1-aros2** is a controlled test variant: the same Folio 0.4.1
  program repackaged as revision 2 with `application.json` in the archive,
  offered by a local test catalogue so an update exists. It is not
  published.
- The pictures beside the details are the **demonstration storefront**
  shipped in PkgManager's `Demo` drawer: real screenshots of Folio and
  GrafX2 taken on this machine the same day, and the programs' own icons.
  They do not come from the catalogue.
- **Example Paint Studio** and its price are fictional; its picture is a
  mockup and says so. "Demo -- purchasing is not available yet".

## Shell: updates, starting a program, a Shell program's return code

![apkg outdated, run --check and run grep](cli-outdated-run.png)

## apkg run folio pages12.pdf: Folio opens the PDF, started from its icon (8 MiB stack, no Stack command)

![Folio started by apkg run with a PDF](apkg-run-folio-pdf.png)

## Limpet: the search table in a wide window, show wrapped in a narrow one, Ctrl-C during a download

![apkg search in Limpet](limpet-search.png)
![apkg show in a narrow Limpet window](limpet-show-narrow.png)
![Ctrl-C during a download in Limpet](limpet-cancel.png)

## PkgManager: the Updates count, Installed, and an application's details with pictures

![The catalogue with one update available](updates.png)
![Installed: Folio with its icon, start line, details and pictures](installed-folio-pictures.png)
![The second picture, chosen by its thumbnail](gallery-second-picture.png)
![grep has no pictures: text only](no-pictures-grep.png)

## Run: GrafX2 started from PkgManager, and the status line after it

![Run started GrafX2](run-grafx2-from-gui.png)
![Started grafx2 from its icon](run-status.png)

## Verify: the result at the top of the details (a file the user added)

![Verify](verify.png)

## Upgrade: the plan first, with a user-changed file kept

![Upgrade preview](upgrade-preview.png)

## Demonstration: Demo store and the purchase preview

![A fictional offer](demo-offer.png)
![Buy: how a purchase could work; nothing is bought](demo-buy-preview.png)

The ScreenBar update counter is an internal demonstration and is not shown
here.
