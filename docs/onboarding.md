# First-run onboarding

The setup app opens `pak_onboarding.json` on the first normal launch. The wizard
appears until the user reaches its last page and presses A. Completion is stored
in `.userdata/shared/Netplay/onboarding-complete`, so pak updates do not repeat
it. Remove that marker to show the wizard again. Quick Menu launches skip it.

Edit the `pages` array to add or reorder pages. Each page needs a short `title`
and can have `description`, `image`, or a setting question. Images are PNG or
JPEG files inside Netplay.pak, referenced by a relative path such as
`res/onboarding/host.png`. Put those image files in `res/onboarding/` in this
repository; packaging copies that folder into Netplay.pak.
The image is scaled to fit above the description. Keep descriptions brief on
small screens (about three lines on question pages). A missing image leaves
more space for text.

A setting question adds `setting` and at least two `options`, each with `label`
and integer `value`. Up and Down select an option; A advances and B returns to
the previous page. Answers are saved together when the wizard finishes. The
allowed settings are `compatibility_cores`, `add_gameswitcher`, `adhoc_switch`,
`verbose_logs` (values 0 or 1). Settings that
install other paks or change the radio immediately remain in Configuration.

Example page with an image and question:

```json
{
  "title": "Game Switcher",
  "description": "Show Netplay in the Game Switcher?",
  "image": "res/onboarding/switcher.png",
  "setting": "add_gameswitcher",
  "options": [
    { "label": "Yes", "value": 1 },
    { "label": "No", "value": 0 }
  ]
}
```
