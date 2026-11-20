#pragma once

#include <cstdint>

/* Contournement d'un bug matériel du PPA (Espressif DIG-734) mal couvert par
 * le driver ESP-IDF 5.4.x dans le cas d'une rotation.
 *
 * Le moteur SRM produit sa sortie par unités de transfert. Quand la dernière
 * unité contient moins de données que la FIFO 2D-DMA (1536 bits), celle-ci
 * n'est jamais vidée : le canal RX ne voit pas d'EOF, la transaction ne se
 * termine JAMAIS. ppa_srm.c contourne le cas en basculant l'ordre des
 * macro-blocs (ppa_ll_srm_bypass_mb_order), mais il décide à partir de la
 * géométrie AVANT rotation (w_out = in.block_w * scale_x). En 270° la sortie
 * réelle fait `h` de large et `w` de haut : le driver peut donc rater le cas.
 *
 * Conséquence d'une transaction sans fin (ppa_core.c) : l'ISR de fin ne tire
 * pas, donc ni le slot du client (ppa_recycle_transaction) ni le sémaphore du
 * MOTEUR SRM ne sont rendus. Toutes les transactions suivantes restent dans
 * trans_stailq sans jamais partir au 2D-DMA — y compris celles du client
 * vidéo, puisque le moteur est partagé. L'appareil reste vivant mais l'écran
 * ne se repeint plus de la session ; ppa_unregister_client refuse même de
 * réinitialiser le client (« still has unprocessed trans »).
 *
 * Ce module est PUR (aucune dépendance ESP-IDF) pour être testable sur host :
 * cf. test/test_ppa_srm_quirk.cpp. */
namespace bsp::ppa_quirk {

/* Vrai si une opération SRM 270° sur une bande LVGL `w` x `h` (RGB565 ->
 * RGB565, échelle 1:1 — le seul cas produit par handle_flush) tombe dans le
 * bug : contournement nécessaire d'après la géométrie tournée, mais non
 * appliqué par le driver d'après la géométrie non tournée. */
[[nodiscard]] bool rot270_band_stalls(std::uint32_t w, std::uint32_t h) noexcept;

/* Nombre maximum de sous-bandes produites par rot270_split : chacune coûte
 * une transaction PPA complète (soumission + attente de l'ISR de fin). */
inline constexpr std::uint32_t kMaxSplitParts = 8;

/* Découpe la bande en sous-bandes horizontales successives dont AUCUNE ne
 * déclenche le bug. Écrit leurs hauteurs dans `heights` (capacité `cap`) et
 * renvoie leur nombre : 1 si la bande est déjà sûre, 0 si aucune découpe
 * connue ne l'est (l'appelant soumet alors la bande telle quelle : une
 * transaction risquée vaut mieux qu'une bande jamais peinte, et le log de
 * timeout donnera sa géométrie).
 *
 * On découpe en hauteur, pas en largeur : c'est la dimension que le décalage
 * de bloc d'entrée exprime directement (in.block_offset_y), sans toucher au
 * pas de ligne de l'image source. */
[[nodiscard]] std::uint32_t rot270_split(std::uint32_t w, std::uint32_t h,
                                         std::uint32_t *heights,
                                         std::uint32_t cap) noexcept;

} // namespace bsp::ppa_quirk
