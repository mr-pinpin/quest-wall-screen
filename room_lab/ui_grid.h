// Layout for in-headset panels, in the spirit of CSS grid: a fixed number of columns, rows created as items
// flow in, sections that start a new row under a small heading, partial rows centred (justify-content: center),
// and one label size for every button. Like min-content sizing, the columns widen until the longest label fits
// at no less than minLabelPx; only past that does the text shrink. The panel's size comes from its content, so adding
// an item never needs hand-placed coordinates. Units are panel-local metres; origin at the panel centre, +y up.
// Text metrics match the app's 5x7 pixel font: a string is (6 * chars - 1) pixels wide and 7 pixels tall.
// Items may carry a hint (what the button does, in plain words): every cell then grows by hintH and shows the hint
// under its label in up to two word-wrapped lines, one hint size for the whole panel.
#pragma once
#include <algorithm>
#include <string>
#include <vector>

struct UiGrid {
    struct Item { std::string label, section, hint; };
    struct Cell { float x, y, w, h, px; float labelY = 0, hintPx = 0; std::string hint1, hint2; float hint1Y = 0, hint2Y = 0; };  // centre, size, label pixel size
    struct Heading { std::string text; float x, y, px; };

    // Style
    int cols = 3;
    float cellW = 0.15f, cellH = 0.075f, gap = 0.02f, pad = 0.025f, sectionGap = 0.012f;
    float titleH = 0.05f, headingH = 0.028f, footerH = 0.05f;  // the footer holds the grab bar
    float labelPx = 0.0055f, minLabelPx = 0.0038f, labelMargin = 0.014f, headingPx = 0.0030f, titlePx = 0.006f;
    float maxCellW = 0.22f;
    float hintH = 0.026f, hintPx = 0.0016f;

    // Result of layout()
    std::vector<Cell> cells;                        // one per item, same order
    std::vector<Heading> headings;
    float halfW = 0, halfH = 0, titleY = 0, barY = 0, colW = 0, px = 0;  // resolved column width and label size

    static float textW(const std::string& s, float px) { return s.empty() ? 0 : (6.0f * s.size() - 1) * px; }
    // A hint in at most two lines, split at the space nearest the middle (one line if it has no space or is short).
    static void wrap(const std::string& s, std::string& a, std::string& b, size_t oneLine = 14) {
        a = s; b.clear();
        if (s.size() <= oneLine) return;
        size_t best = std::string::npos;
        for (size_t i = 0; i < s.size(); i++) if (s[i] == ' ' && (best == std::string::npos || std::abs((long)i * 2 - (long)s.size()) < std::abs((long)best * 2 - (long)s.size()))) best = i;
        if (best == std::string::npos) return;
        a = s.substr(0, best); b = s.substr(best + 1);
    }
    // Largest pixel size <= want that fits the text in maxW x maxH.
    static float fit(const std::string& s, float want, float maxW, float maxH) {
        float px = want;
        if (!s.empty()) px = std::min(px, maxW / (6.0f * s.size() - 1));
        return std::min(px, maxH / 7.0f);
    }

    void layout(const std::vector<Item>& items) {
        cells.assign(items.size(), {});
        headings.clear();
        // One label size for all buttons; widen the columns (up to maxCellW) before shrinking text below minLabelPx.
        size_t longest = 0; for (auto& it : items) longest = std::max(longest, it.label.size());
        float need = longest ? 6.0f * longest - 1 : 1;
        colW = std::min(maxCellW, std::max(cellW, need * minLabelPx + 2 * labelMargin));
        px = std::min({labelPx, (colW - 2 * labelMargin) / need, cellH * 0.55f / 7.0f});
        // Group consecutive items by section, then break each section into rows of `cols`.
        struct Row { std::vector<int> idx; bool first; std::string section; };
        std::vector<Row> rows;
        for (int i = 0; i < (int)items.size(); i++) {
            bool newSection = rows.empty() || items[i].section != rows.back().section;
            if (newSection || (int)rows.back().idx.size() == cols) rows.push_back({{}, newSection, items[i].section});
            rows.back().idx.push_back(i);
        }
        bool hints = false; for (auto& it : items) hints = hints || !it.hint.empty();
        const float ch = cellH + (hints ? hintH : 0);  // the cell: label area on top, hint area under it
        float hpx = hintPx;  // one hint size for the panel: the largest that fits every hint's lines
        for (auto& it : items) {
            std::string a, b; wrap(it.hint, a, b);
            for (auto* l : {&a, &b}) if (!l->empty()) hpx = std::min(hpx, fit(*l, hintPx, colW - 2 * labelMargin * 0.5f, hintH * 0.42f));
        }
        float width = 2 * pad + cols * colW + (cols - 1) * gap, height = 2 * pad + titleH + footerH;
        for (size_t r = 0; r < rows.size(); r++) {
            if (rows[r].first) height += (r ? sectionGap : 0) + (rows[r].section.empty() ? 0 : headingH);
            else height += gap;
            height += ch;
        }
        halfW = width / 2; halfH = height / 2;
        float y = halfH - pad;
        titleY = y - titleH / 2; y -= titleH;
        for (size_t r = 0; r < rows.size(); r++) {
            if (rows[r].first) {
                if (r) y -= sectionGap;
                if (!rows[r].section.empty()) {
                    float px = fit(rows[r].section, headingPx, width - 2 * pad, headingH * 0.7f);
                    headings.push_back({rows[r].section, -halfW + pad + textW(rows[r].section, px) / 2, y - headingH / 2, px});
                    y -= headingH;
                }
            } else y -= gap;
            int n = (int)rows[r].idx.size();
            float x = -(n * colW + (n - 1) * gap) / 2 + colW / 2, cy = y - ch / 2;
            for (int i : rows[r].idx) {
                Cell c{x, cy, colW, ch, px};
                c.labelY = hints ? cy + ch / 2 - cellH / 2 : cy;
                if (hints) {
                    wrap(items[i].hint, c.hint1, c.hint2); c.hintPx = hpx;
                    float mid = cy - ch / 2 + hintH / 2 + 0.004f, step = 9 * hpx;
                    if (c.hint2.empty()) c.hint1Y = mid; else { c.hint1Y = mid + step / 2; c.hint2Y = mid - step / 2; }
                }
                cells[i] = c; x += colW + gap;
            }
            y -= ch;
        }
        barY = -halfH + pad + footerH / 2 - 0.01f;
    }
    // Index of the item under (x, y), or -1.
    int hit(float x, float y) const {
        for (size_t i = 0; i < cells.size(); i++)
            if (std::abs(x - cells[i].x) < cells[i].w / 2 && std::abs(y - cells[i].y) < cells[i].h / 2) return (int)i;
        return -1;
    }
};
