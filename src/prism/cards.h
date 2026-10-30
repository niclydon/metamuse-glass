/* prismd: draw one NEXUS-LINK §2 card on the 640x360 canvas as the `prism` surface
 * (narrow limits: title ≤ 24, body ≤ 3 lines of ≤ 40). Every card type has a
 * layout; `approve` draws a refusal frame because approve cards never travel the
 * node socket (prismd drops them before this is reached). Text that exceeds the
 * surface limits is cut with an ellipsis rather than overflowing. */
#ifndef PRISM_CARDS_H
#define PRISM_CARDS_H
#include "fb.h"
#include "json.h"

typedef struct {
    int page;      /* page cards: which of action.pages is shown (0-based) */
    int selected;  /* choose cards: highlighted option (0-based) */
} card_view;

/* attention is the host's current attention state (ambient/glance/engaged/interrupt)
 * or NULL; it colours the top bar. */
void cards_render(fb_t *fb, const jnode *card, const card_view *view, const char *attention);
/* Number of pages for a page card (1 otherwise) and options for a choose card (0 otherwise). */
int cards_page_count(const jnode *card);
int cards_option_count(const jnode *card);
#endif
