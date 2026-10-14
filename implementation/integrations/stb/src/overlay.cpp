#include "overlay.hpp"

#include <algorithm>
#include <string>

namespace atsc3::stb {

namespace {

void setFont(cairo_t* cr, double size, bool bold) {
    cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, bold ? CAIRO_FONT_WEIGHT_BOLD : CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, size);
}

double textWidth(cairo_t* cr, const std::string& text) {
    cairo_text_extents_t ext;
    cairo_text_extents(cr, text.c_str(), &ext);
    return ext.x_advance;
}

/// Cuts `text` so that it fits `max_width` at the current font, appending an ellipsis when cut (UTF-8 aware).
std::string fitText(cairo_t* cr, std::string text, double max_width) {
    if (textWidth(cr, text) <= max_width) return text;
    const std::string ellipsis = "\xE2\x80\xA6";
    while (!text.empty()) {
        std::size_t cut = text.size() - 1;
        while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80) --cut;   // back to a code point start
        text.erase(cut);
        if (textWidth(cr, text + ellipsis) <= max_width) return text + ellipsis;
    }
    return ellipsis;
}

void text(cairo_t* cr, double x, double y, const std::string& s, double max_width = 0.0) {
    cairo_move_to(cr, x, y);
    cairo_show_text(cr, max_width > 0.0 ? fitText(cr, s, max_width).c_str() : s.c_str());
}

void panel(cairo_t* cr, double x, double y, double w, double h, double alpha) {
    cairo_set_source_rgba(cr, 0.04, 0.05, 0.09, alpha);
    cairo_rectangle(cr, x, y, w, h);
    cairo_fill(cr);
}

void drawList(cairo_t* cr, int width, int height, const UiSnapshot& ui) {
    const double w = width / 2.0, h = height;
    panel(cr, 0, 0, w, h, 0.82);
    const double margin = w * 0.06;
    const double title_size = h / 18.0, row_size = h / 26.0, row_h = row_size * 1.9;
    setFont(cr, title_size, true);
    cairo_set_source_rgb(cr, 1, 1, 1);
    text(cr, margin, margin + title_size, "Channels");
    cairo_set_source_rgba(cr, 1, 1, 1, 0.35);
    cairo_set_line_width(cr, 2);
    cairo_move_to(cr, margin, margin + title_size * 1.6);
    cairo_line_to(cr, w - margin, margin + title_size * 1.6);
    cairo_stroke(cr);

    const double top = margin + title_size * 2.1;
    const int rows = std::max(1, static_cast<int>((h - top - margin) / row_h));
    const int n = static_cast<int>(ui.channels.size());
    int first = 0;
    if (ui.highlight >= rows) first = std::min(ui.highlight - rows + 1, std::max(0, n - rows));
    setFont(cr, row_size, false);
    const double number_w = textWidth(cr, "888.88") + row_size * 0.6;
    for (int i = first; i < n && i < first + rows; ++i) {
        const Channel& c = ui.channels[static_cast<std::size_t>(i)];
        const double y = top + (i - first) * row_h;
        if (i == ui.highlight) {
            cairo_set_source_rgba(cr, 0.95, 0.75, 0.15, 0.9);
            cairo_rectangle(cr, margin * 0.5, y, w - margin, row_h);
            cairo_fill(cr);
            cairo_set_source_rgb(cr, 0.05, 0.05, 0.05);
        } else {
            cairo_set_source_rgb(cr, 1, 1, 1);
        }
        const double baseline = y + row_h * 0.68;
        if (i == ui.tuned) {   // playing marker: a small triangle
            cairo_move_to(cr, margin * 0.7, baseline - row_size * 0.75);
            cairo_line_to(cr, margin * 0.7 + row_size * 0.55, baseline - row_size * 0.35);
            cairo_line_to(cr, margin * 0.7, baseline + row_size * 0.05);
            cairo_close_path(cr);
            cairo_fill(cr);
        }
        setFont(cr, row_size, i == ui.tuned);
        text(cr, margin * 0.7 + row_size * 0.9, baseline, c.number());
        std::string name = c.name;
        if (c.protected_flag) name += " (DRM)";
        if (c.hidden) name += " (hidden)";
        text(cr, margin * 0.7 + row_size * 0.9 + number_w, baseline, name, w - margin * 1.6 - row_size * 0.9 - number_w);
    }
    if (n == 0) {
        cairo_set_source_rgba(cr, 1, 1, 1, 0.7);
        text(cr, margin, top + row_size, "No services yet");
    }
    if (n > rows) {   // scroll position
        const double track_top = top, track_h = rows * row_h;
        cairo_set_source_rgba(cr, 1, 1, 1, 0.25);
        cairo_rectangle(cr, w - margin * 0.35, track_top, margin * 0.12, track_h);
        cairo_fill(cr);
        cairo_set_source_rgba(cr, 1, 1, 1, 0.8);
        cairo_rectangle(cr, w - margin * 0.35, track_top + track_h * first / n, margin * 0.12, std::max(row_h, track_h * rows / n));
        cairo_fill(cr);
    }
}

void drawBanner(cairo_t* cr, int width, int height, const UiSnapshot& ui) {
    const double w = width, h = height;
    const double bh = h * 0.26, by = h - bh;
    panel(cr, 0, by, w, bh, 0.85);
    const double margin = w * 0.03;
    const double big = bh * 0.27, small = bh * 0.15;
    double y = by + bh * 0.30;
    cairo_set_source_rgb(cr, 1, 1, 1);
    setFont(cr, big, true);
    if (ui.channel) {
        const std::string number = ui.channel->number();
        text(cr, margin, y, number);
        const double nw = textWidth(cr, number) + big * 0.5;
        std::string name = ui.channel->name;
        if (ui.channel->protected_flag) name += " (DRM)";
        text(cr, margin + nw, y, name, w * 0.62 - nw);
    } else {
        text(cr, margin, y, "No channel");
    }
    // the clock and the status on the right
    setFont(cr, small, false);
    cairo_set_source_rgba(cr, 1, 1, 1, 0.8);
    const std::string clock = ui.now_unix > 0 ? clockText(ui.now_unix) : std::string();
    if (!clock.empty()) text(cr, w - margin - textWidth(cr, clock), y, clock);
    if (!ui.status.empty()) {
        cairo_set_source_rgba(cr, 0.95, 0.75, 0.15, 0.95);
        const std::string st = fitText(cr, ui.status, w * 0.3);
        text(cr, w - margin - textWidth(cr, st), y + small * 1.5, st);
    }
    // present / following
    y = by + bh * 0.52;
    setFont(cr, small, false);
    const double label_w = textWidth(cr, "Next  ") + small * 0.3;
    const double time_w = textWidth(cr, "00:00 – 00:00") + small * 0.6;
    if (ui.epg.now) {
        const Programme& p = *ui.epg.now;
        cairo_set_source_rgba(cr, 1, 1, 1, 0.75);
        text(cr, margin, y, "Now");
        text(cr, margin + label_w, y, clockText(p.start_unix) + " \xE2\x80\x93 " + clockText(p.end_unix));
        cairo_set_source_rgb(cr, 1, 1, 1);
        setFont(cr, small, true);
        text(cr, margin + label_w + time_w, y, p.title, w * 0.66 - label_w - time_w);
        setFont(cr, small, false);
        // progress of the present programme
        const double bar_y = by + bh * 0.58, bar_w = w * 0.66, bar_h = bh * 0.035;
        const double frac = p.end_unix > p.start_unix ? std::clamp(static_cast<double>(ui.now_unix - p.start_unix) / static_cast<double>(p.end_unix - p.start_unix), 0.0, 1.0) : 0.0;
        cairo_set_source_rgba(cr, 1, 1, 1, 0.25);
        cairo_rectangle(cr, margin, bar_y, bar_w, bar_h);
        cairo_fill(cr);
        cairo_set_source_rgba(cr, 0.95, 0.75, 0.15, 0.95);
        cairo_rectangle(cr, margin, bar_y, bar_w * frac, bar_h);
        cairo_fill(cr);
    } else {
        cairo_set_source_rgba(cr, 1, 1, 1, 0.6);
        text(cr, margin, y, ui.channel ? "No programme information" : "");
    }
    y = by + bh * 0.80;
    if (ui.epg.next) {
        const Programme& p = *ui.epg.next;
        cairo_set_source_rgba(cr, 1, 1, 1, 0.75);
        text(cr, margin, y, "Next");
        text(cr, margin + label_w, y, clockText(p.start_unix) + " \xE2\x80\x93 " + clockText(p.end_unix));
        cairo_set_source_rgba(cr, 1, 1, 1, 0.9);
        text(cr, margin + label_w + time_w, y, p.title, w * 0.66 - label_w - time_w);
    }
    // codecs, bottom right
    std::string codecs;
    if (!ui.video_codec.empty()) codecs += ui.video_codec;
    if (!ui.audio_codec.empty()) codecs += (codecs.empty() ? "" : "  ") + ui.audio_codec;
    if (!codecs.empty()) {
        setFont(cr, small * 0.8, false);
        cairo_set_source_rgba(cr, 1, 1, 1, 0.55);
        text(cr, w - margin - textWidth(cr, codecs), by + bh * 0.95, codecs);
    }
}

void drawStatusPill(cairo_t* cr, int width, int height, const std::string& status) {
    const double size = height / 30.0, margin = width * 0.015;
    setFont(cr, size, false);
    const double tw = textWidth(cr, status);
    panel(cr, margin, margin, tw + size * 1.2, size * 1.8, 0.75);
    cairo_set_source_rgba(cr, 0.95, 0.75, 0.15, 0.95);
    text(cr, margin + size * 0.6, margin + size * 1.25, status);
}

}  // namespace

void drawUi(cairo_t* cr, int width, int height, const UiSnapshot& ui) {
    if (ui.list_visible) drawList(cr, width, height, ui);
    if (ui.banner_visible) drawBanner(cr, width, height, ui);
    else if (!ui.status.empty()) drawStatusPill(cr, width, height, ui.status);
}

}  // namespace atsc3::stb
