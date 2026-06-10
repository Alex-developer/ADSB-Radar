#pragma once

/*
 * Bootstrap settings page served from flash.
 *
 * The HTML, CSS, and JavaScript live in the .cpp file as one raw string so the
 * ESP can serve the page without a filesystem dependency.
 */
extern const char SETTINGS_PAGE_HTML[];
