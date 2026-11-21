/*
Griddy - one-click source layouts for OBS Studio
Copyright (C) 2026 Voidscape Development

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License along
with this program. If not, see <https://www.gnu.org/licenses/>
*/

#include "layout.hpp"
#include <QtWidgets> // before OBS headers to avoid SIMDe intrinsic clashes
#include <obs-frontend-api.h>
#include <obs-module.h>
#include <plugin-support.h>
#include <util/config-file.h>
#include <functional>
#include <numeric>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE(PLUGIN_NAME, "en-US")

static std::function<void()> g_layoutsChanged; // editor -> dock

static QString g_canvasName; // target canvas (e.g. a Vertical Canvas), empty = main

static obs_canvas_t *targetCanvas(const QString &name = g_canvasName)
{
	obs_canvas_t *c = name.isEmpty() ? nullptr : obs_get_canvas_by_name(name.toUtf8().constData());
	return c ? c : obs_get_main_canvas();
}

static QSizeF canvasSize(const QString &name = g_canvasName)
{
	obs_canvas_t *c = targetCanvas(name);
	obs_video_info o;
	const bool ok = obs_canvas_get_flags(c) & MAIN ? obs_get_video_info(&o) : obs_canvas_get_video_info(c, &o);
	obs_canvas_release(c);
	return ok ? QSizeF(o.base_width, o.base_height) : QSizeF(1920, 1080);
}

// Size a layout is drawn on: the target canvas, turned if the layout is made for the other orientation.
static QSizeF layoutSize(const Layout &L)
{
	const QSizeF c = canvasSize();
	return L.fits(c.width(), c.height()) ? c : c.transposed();
}

// Maps canvas pixels into a widget area, keeping the canvas aspect ratio.
struct View {
	QRectF canvas;
	QSizeF size;
	double s;
	View(const QRectF &area, const QSizeF &c) : size(c)
	{
		canvas = QRectF(QPointF(), c.scaled(area.size(), Qt::KeepAspectRatio));
		canvas.moveCenter(area.center());
		s = canvas.width() / c.width();
	}
	QRectF map(const QRectF &r) const { return {canvas.topLeft() + r.topLeft() * s, r.size() * s}; }
};

static QVector<QRectF> computeOnCanvas(const Layout &L)
{
	const QSizeF c = layoutSize(L);
	return L.compute(c.width(), c.height());
}

// Shades the parts of the canvas covered by the platform's UI.
static void drawSafeZone(QPainter &p, const View &v, const Layout &L)
{
	if (L.safeZone <= 0)
		return;
	const QRectF safe = v.map(L.safeRect(v.size.width(), v.size.height()));
	QPainterPath path;
	path.addRect(v.canvas);
	path.addRect(safe);
	p.fillPath(path, QColor(255, 60, 60, 70));
	p.setPen(QPen(QColor(255, 110, 110), 1, Qt::DotLine));
	p.setBrush(Qt::NoBrush);
	p.drawRect(safe);
}

static const QColor kColors[] = {0x4c8bf5, 0xf59e0b, 0x10b981, 0xec4899, 0x8b5cf6, 0x06b6d4}; // per slot

// Slots numbered from 1; `labels` adds a second line (e.g. the source name) under each number.
static void drawSlots(QPainter &p, const View &v, const QVector<QRectF> &rects, int sel = -1, bool outline = false,
		      const QStringList &labels = {})
{
	p.setRenderHint(QPainter::Antialiasing);
	for (int i = 0; i < rects.size(); i++) {
		const QRectF q = v.map(rects[i]);
		if (outline) {
			p.setPen(QPen(Qt::white, 1, Qt::DashLine));
			p.setBrush(Qt::NoBrush);
			p.drawRect(q);
			continue;
		}
		QColor c = kColors[i % 6];
		p.setPen(QPen(i == sel ? Qt::white : c.lighter(130), i == sel ? 2.5 : 1));
		c.setAlpha(170);
		p.setBrush(c);
		p.drawRect(q);
		const QString label = labels.value(i);
		QFont f = p.font();
		f.setPixelSize(qBound(8, int(q.height() * (label.isEmpty() ? 0.35 : 0.16)), label.isEmpty() ? 32 : 14));
		f.setBold(true);
		p.setFont(f);
		p.setPen(Qt::white);
		p.drawText(q, Qt::AlignCenter,
			   QString::number(i + 1) + (label.isEmpty()
							     ? ""
							     : "\n" + p.fontMetrics().elidedText(label, Qt::ElideRight,
												 int(q.width()) - 4)));
	}
}

static QIcon layoutIcon(const Layout &L)
{
	const QSizeF c = layoutSize(L);
	QPixmap pm(c.scaled(128, 128, Qt::KeepAspectRatio).toSize());
	pm.fill(Qt::transparent);
	{
		QPainter p(&pm);
		const View v(pm.rect(), c);
		p.fillRect(v.canvas, QColor(30, 30, 36));
		drawSlots(p, v, computeOnCanvas(L));
		drawSafeZone(p, v, L);
	}
	return pm;
}

/* ---------- Interactive layout canvas (editor) ---------- */

class LayoutCanvas : public QWidget {
public:
	Layout *L = nullptr;
	int sel = -1;
	std::function<void()> changed, selected;

	LayoutCanvas()
	{
		setMinimumSize(480, 270);
		setFocusPolicy(Qt::ClickFocus);
	}
	void select(int i)
	{
		sel = i;
		if (selected)
			selected();
		update();
	}

private:
	enum { None, Cells, Move, Resize, New } mode = None;
	QPointF press, start;
	QPoint cellA, cellB;
	QRectF orig;
	bool moved = false;

	View view() const { return View(rect(), layoutSize(*L)); }
	// Editing works on the plain layout; aspect-locked / safe-zone results are drawn dashed on top.
	Layout base() const
	{
		Layout b = *L;
		b.aspectLock = b.mirror = b.safeFit = false;
		return b;
	}
	QRectF content() const
	{
		const View v = view();
		const double m = L->margin * v.s;
		return v.canvas.adjusted(m, m, -m, -m);
	}
	QVector<QRectF> shown() const
	{
		const View v = view();
		QVector<QRectF> out;
		for (const auto &r : computeOnCanvas(base()))
			out += v.map(r);
		return out;
	}
	int slotAt(QPointF p) const
	{
		const auto s = shown();
		for (auto i = s.size() - 1; i >= 0; i--)
			if (s[i].contains(p))
				return int(i);
		return -1;
	}
	QPointF norm(QPointF p) const
	{
		const QRectF c = content();
		auto snap = [](double v) {
			return qBound(0.0, std::round(v * 48) / 48, 1.0);
		};
		return {snap((p.x() - c.left()) / c.width()), snap((p.y() - c.top()) / c.height())};
	}
	static double frac(const QVector<double> &v, int i)
	{
		return std::accumulate(v.begin(), v.begin() + i, 0.0) / std::accumulate(v.begin(), v.end(), 0.0);
	}
	static int index(const QVector<double> &v, double f)
	{
		int i = 0;
		while (i < v.size() - 1 && frac(v, i + 1) <= f)
			i++;
		return i;
	}
	QPoint cellAt(QPointF p) const
	{
		const QRectF c = content();
		return {index(L->cols, (p.x() - c.left()) / c.width()), index(L->rows, (p.y() - c.top()) / c.height())};
	}
	QRectF cellsRect(const QRect &r) const
	{
		const QRectF c = content();
		return {QPointF(c.left() + frac(L->cols, r.left()) * c.width(),
				c.top() + frac(L->rows, r.top()) * c.height()),
			QPointF(c.left() + frac(L->cols, r.right() + 1) * c.width(),
				c.top() + frac(L->rows, r.bottom() + 1) * c.height())};
	}

	void paintEvent(QPaintEvent *) override
	{
		QPainter p(this);
		if (!L)
			return;
		const View v = view();
		p.fillRect(v.canvas, QColor(30, 30, 36));
		if (L->grid) {
			const QRectF c = content();
			p.setPen(QColor(90, 90, 100));
			for (int i = 1; i < L->cols.size(); i++)
				p.drawLine(QPointF(c.left() + frac(L->cols, i) * c.width(), c.top()),
					   QPointF(c.left() + frac(L->cols, i) * c.width(), c.bottom()));
			for (int i = 1; i < L->rows.size(); i++)
				p.drawLine(QPointF(c.left(), c.top() + frac(L->rows, i) * c.height()),
					   QPointF(c.right(), c.top() + frac(L->rows, i) * c.height()));
			p.drawRect(c);
		}
		drawSlots(p, v, computeOnCanvas(base()), sel);
		if (L->aspectLock || L->safeFit) {
			Layout f = *L;
			f.mirror = false;
			drawSlots(p, v, computeOnCanvas(f), -1, true);
		}
		drawSafeZone(p, v, *L);
		if (mode == Cells && moved) {
			p.setPen(QPen(Qt::white, 2));
			p.setBrush(QColor(255, 255, 255, 50));
			p.drawRect(cellsRect(QRect(cellA, cellB).normalized()));
		}
	}

	void mousePressEvent(QMouseEvent *e) override
	{
		if (!L)
			return;
		press = e->position();
		moved = false;
		const int hit = slotAt(press);
		select(hit);
		if (L->grid) {
			mode = content().contains(press) ? Cells : None;
			cellA = cellB = cellAt(press);
		} else if (hit >= 0) {
			mode = (shown()[hit].bottomRight() - press).manhattanLength() < 14 ? Resize : Move;
			orig = L->slots[hit].r;
		} else {
			mode = New;
			start = norm(press);
			L->slots += Slot{QRectF(start, start), FitCrop, 0};
			select(int(L->slots.size()) - 1);
		}
	}

	void mouseMoveEvent(QMouseEvent *e) override
	{
		if (mode == None || (!moved && (e->position() - press).manhattanLength() < 5))
			return;
		moved = true;
		const QPointF n = norm(e->position());
		if (mode == Cells) {
			cellB = cellAt(e->position());
		} else if (mode == Move) {
			QRectF r = orig.translated(n - norm(press));
			r.moveTo(qBound(0.0, r.x(), 1 - r.width()), qBound(0.0, r.y(), 1 - r.height()));
			L->slots[sel].r = r;
		} else {
			L->slots[sel].r = QRectF(mode == New ? start : orig.topLeft(), n).normalized();
		}
		update();
	}

	void mouseReleaseEvent(QMouseEvent *) override
	{
		if (mode == None)
			return;
		auto &s = L->slots;
		bool dirty = moved;
		if (mode == Cells && (moved || sel < 0)) {
			// Click on an empty cell adds a slot; dragging merges the cells, replacing what it covers
			const QRect span = QRect(cellA, cellB).normalized();
			for (auto i = s.size() - 1; i >= 0; i--)
				if (s[i].r.toRect().intersects(span))
					s.removeAt(i);
			s += Slot{QRectF(span), FitCrop, 0};
			select(int(s.size()) - 1);
			dirty = true;
		} else if (mode == Cells) {
			dirty = false;
		} else if (sel >= 0 && (s[sel].r.width() < 0.02 || s[sel].r.height() < 0.02)) {
			if (mode == New)
				s.removeAt(sel), select(-1);
			else
				s[sel].r = orig;
		}
		mode = None;
		if (dirty && changed)
			changed();
		update();
	}

	void keyPressEvent(QKeyEvent *e) override
	{
		if (L && sel >= 0 && (e->key() == Qt::Key_Delete || e->key() == Qt::Key_Backspace)) {
			L->slots.removeAt(sel);
			select(-1);
			changed();
		}
	}
};

/* ---------- Layout editor dialog ---------- */

static const QList<QPair<QString, double>> kAspects = {{"Off", 0}, {"16:9", 16.0 / 9}, {"4:3", 4.0 / 3},
						       {"1:1", 1}, {"9:16", 9.0 / 16}, {"21:9", 21.0 / 9}};

static QVector<double> parseWeights(const QString &s)
{
	QVector<double> v;
	for (const auto &x : s.split(',', Qt::SkipEmptyParts))
		if (double d = x.trimmed().toDouble(); d > 0)
			v += d;
	return v.isEmpty() ? QVector<double>{1} : v;
}

static QString weightsText(const QVector<double> &v)
{
	QStringList l;
	for (double d : v)
		l += QString::number(d, 'g', 4);
	return l.join(", ");
}

static QSpinBox *spin(int max, const QString &suffix = " px")
{
	auto *s = new QSpinBox;
	s->setRange(0, max);
	s->setSuffix(suffix);
	return s;
}

// Slider with a value readout; returns the row widget so the row can be hidden.
static QWidget *sliderRow(QSlider *&s, int lo, int hi, const QString &suffix)
{
	auto *w = new QWidget;
	auto *l = new QHBoxLayout(w);
	auto *v = new QLabel(QString::number(lo) + suffix);
	s = new QSlider(Qt::Horizontal);
	s->setRange(lo, hi);
	v->setMinimumWidth(v->fontMetrics().horizontalAdvance("800 px"));
	v->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
	QObject::connect(s, &QSlider::valueChanged, v, [v, suffix](int x) { v->setText(QString::number(x) + suffix); });
	l->setContentsMargins({});
	l->addWidget(s, 1);
	l->addWidget(v);
	return w;
}

static QToolButton *toggle(const QString &text, const QString &tip)
{
	auto *b = new QToolButton;
	b->setText(text);
	b->setToolTip(tip);
	b->setCheckable(true);
	return b;
}

static QComboBox *safeZoneBox()
{
	auto *c = new QComboBox;
	for (const auto &z : kSafeZones)
		c->addItem(z.name);
	c->setToolTip("Shade the area covered by the app's buttons and captions (approximate)");
	return c;
}

// Row of exclusive toggle buttons. The group's idClicked fires on user clicks only.
class Segmented : public QWidget {
public:
	QButtonGroup group;
	explicit Segmented(const QStringList &items = {})
	{
		auto *l = new QHBoxLayout(this);
		l->setContentsMargins({});
		l->setSpacing(0);
		setItems(items);
	}
	void setItems(const QStringList &items)
	{
		qDeleteAll(group.buttons());
		for (int i = 0; i < items.size(); i++) {
			auto *b = new QToolButton;
			b->setText(items[i]);
			b->setCheckable(true);
			b->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
			layout()->addWidget(b);
			group.addButton(b, i);
		}
	}
	int index() const { return group.checkedId(); }
	void setIndex(int i)
	{
		if (auto *b = group.button(i))
			b->setChecked(true);
	}
};

/* ---------- Grouped layout list (dock gallery and editor list) ---------- */

class LayoutList : public QListWidget {
public:
	QString filter;
	std::function<bool(const Layout &)> show; // extra filter, e.g. the canvas orientation

	int layoutAt(const QListWidgetItem *it) const
	{
		return it && it->data(Qt::UserRole).isValid() ? it->data(Qt::UserRole).toInt() : -1;
	}
	int current() const { return layoutAt(currentItem()); }
	LayoutList() { setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff); }
	// Rebuilds the list under group headers and selects layout `sel`, or the first one shown.
	void fill(int sel)
	{
		const QSignalBlocker block(this);
		clear();
		QStringList groups;
		for (const auto &L : layouts())
			if (!groups.contains(L.group))
				groups += L.group;
		QListWidgetItem *pick = nullptr;
		for (const auto &g : groups) {
			auto *h = new QListWidgetItem(g.toUpper(), this);
			h->setFlags(Qt::NoItemFlags);
			h->setTextAlignment(Qt::AlignLeft | Qt::AlignBottom);
			QFont f = font();
			f.setBold(true);
			h->setFont(f);
			bool any = false;
			for (int i = 0; i < layouts().size(); i++) {
				const Layout &L = layouts()[i];
				if (L.group != g || !L.name.contains(filter, Qt::CaseInsensitive) || (show && !show(L)))
					continue;
				auto *it = new QListWidgetItem(layoutIcon(L), L.name, this);
				it->setData(Qt::UserRole, i);
				if (viewMode() == IconMode)
					it->setSizeHint(iconSize() + QSize(12, 2 * fontMetrics().height() + 10));
				if (!pick || i == sel)
					pick = it;
				any = true;
			}
			if (!any)
				delete h;
		}
		fitHeaders();
		setCurrentItem(pick);
	}

protected:
	void resizeEvent(QResizeEvent *e) override
	{
		QListWidget::resizeEvent(e);
		fitHeaders();
	}

private:
	void fitHeaders() // headers span a whole row of the icon grid
	{
		for (int i = 0; viewMode() == IconMode && i < count(); i++)
			if (layoutAt(item(i)) < 0)
				item(i)->setSizeHint(
					QSize(viewport()->width() - 2 * spacing() - 2, fontMetrics().height() + 10));
	}
};

class EditorDialog : public QDialog {
	LayoutList *list = new LayoutList;
	LayoutCanvas *canvas = new LayoutCanvas;
	QLineEdit *name = new QLineEdit, *cols = new QLineEdit, *rows = new QLineEdit, *filter = new QLineEdit;
	QComboBox *group = new QComboBox, *safe = safeZoneBox(), *aspect = new QComboBox;
	Segmented *mode = new Segmented({"Grid", "Free"}), *orient = new Segmented({"Any", "Landscape", "Portrait"}),
		  *fit = new Segmented({"Crop to fill", "Fit inside", "Stretch"});
	QSlider *gap = nullptr, *margin = nullptr;
	QSpinBox *order = spin(1, "");
	QToolButton *lock = toggle("Aspect lock", "Keep each slot's locked aspect ratio"),
		    *mirror = toggle("Mirror", "Flip the layout left/right");
	QCheckBox *safeFit = new QCheckBox("Keep slots inside safe zone");
	QTabWidget *tabs = new QTabWidget;
	QWidget *bar = new QWidget, *layoutTab = new QWidget, *slotTab = new QWidget, *slotBox = new QWidget;
	QLabel *help = new QLabel,
	       *slotHint = new QLabel("Click a slot on the canvas to edit its fit, aspect and order.");
	bool loading = false;

	Layout *cur()
	{
		const int i = list->current();
		return i >= 0 && i < layouts().size() ? &layouts()[i] : nullptr;
	}
	void commit()
	{
		saveLayouts();
		if (auto *it = list->currentItem(); it && cur()) {
			it->setText(cur()->name);
			it->setIcon(layoutIcon(*cur()));
		}
		canvas->update();
		if (g_layoutsChanged)
			g_layoutsChanged();
	}
	// Wraps a layout/slot mutation into a slot callback that ignores programmatic updates.
	std::function<void()> edit(std::function<void(Layout &)> fn)
	{
		return [this, fn] {
			if (Layout *L = cur(); L && !loading) {
				fn(*L);
				commit();
			}
		};
	}
	std::function<void()> editSlot(std::function<void(Layout &, Slot &)> fn)
	{
		return edit([this, fn](Layout &L) {
			if (canvas->sel >= 0 && canvas->sel < L.slots.size())
				fn(L, L.slots[canvas->sel]);
		});
	}
	void loadLayout()
	{
		Layout *L = cur();
		loading = true;
		canvas->L = L;
		canvas->sel = -1;
		bar->setEnabled(L);
		layoutTab->setEnabled(L);
		if (L) {
			name->setText(L->name);
			group->clear();
			for (const auto &x : layouts())
				if (group->findText(x.group) < 0)
					group->addItem(x.group);
			group->setCurrentText(L->group);
			mode->setIndex(L->grid ? 0 : 1);
			orient->setIndex(L->orient);
			cols->setText(weightsText(L->cols));
			rows->setText(weightsText(L->rows));
			cols->setEnabled(L->grid);
			rows->setEnabled(L->grid);
			gap->setValue(L->gap);
			margin->setValue(L->margin);
			lock->setChecked(L->aspectLock);
			mirror->setChecked(L->mirror);
			safe->setCurrentIndex(L->safeZone);
			safeFit->setChecked(L->safeFit);
			help->setText(
				L->grid ? "Click a cell to add a slot  ·  Drag across cells to merge  ·  Del removes the "
					  "selected slot  ·  Dashed outlines show the result with aspect lock"
					: "Drag to draw a slot  ·  Drag a slot to move it  ·  Drag its corner to resize  "
					  "·  Del removes the selected slot");
		}
		loading = false;
		loadSlot();
		tabs->setCurrentWidget(layoutTab);
	}
	void loadSlot()
	{
		Layout *L = cur();
		const int i = canvas->sel;
		const bool ok = L && i >= 0 && i < L->slots.size();
		slotBox->setVisible(ok);
		slotHint->setVisible(!ok);
		loading = true;
		if (ok) {
			tabs->setTabText(1, QString("Slot %1").arg(i + 1));
			fit->setIndex(L->slots[i].fit);
			aspect->setCurrentIndex(std::max(0, aspect->findData(L->slots[i].aspect)));
			order->setRange(1, int(L->slots.size()));
			order->setValue(i + 1);
		} else {
			tabs->setTabText(1, "Slot");
		}
		loading = false;
		canvas->update();
	}

public:
	explicit EditorDialog(QWidget *parent) : QDialog(parent)
	{
		setWindowTitle("Griddy Layout Editor");
		resize(1180, 660);
		list->setIconSize(QSize(64, 64));
		group->setEditable(true);
		group->setToolTip("Section the layout is listed under in the dock");
		orient->setToolTip("The Griddy dock only lists layouts that match the target canvas");
		for (const auto &[n, a] : kAspects)
			aspect->addItem(n, a);
		name->setPlaceholderText("Layout name");
		name->setMinimumWidth(220);
		QFont bold = name->font();
		bold.setBold(true);
		name->setFont(bold);
		cols->setPlaceholderText("e.g. 2, 1");
		rows->setPlaceholderText("e.g. 1, 1");
		filter->setPlaceholderText("Filter layouts");
		filter->setClearButtonEnabled(true);
		help->setWordWrap(true);
		help->setEnabled(false); // dimmed hint text
		slotHint->setWordWrap(true);

		// Toolbar: what the layout is
		auto *bl = new QHBoxLayout(bar);
		bl->setContentsMargins({});
		bl->addWidget(name);
		bl->addWidget(mode);
		bl->addWidget(orient);
		mode->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Preferred);
		orient->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Preferred);
		auto *dup = new QToolButton, *more = new QToolButton;
		dup->setText("Duplicate");
		more->setText("More");
		more->setPopupMode(QToolButton::InstantPopup);
		auto *menu = new QMenu(more);
		more->setMenu(menu);
		auto *top = new QHBoxLayout;
		top->addWidget(bar);
		top->addStretch();
		top->addWidget(dup);
		top->addWidget(more);

		// Left: layout list
		auto *newBtn = new QPushButton("+ New Layout");
		auto *left = new QWidget;
		auto *ll = new QVBoxLayout(left);
		ll->setContentsMargins({});
		ll->addWidget(newBtn);
		ll->addWidget(filter);
		ll->addWidget(list, 1);
		left->setFixedWidth(240);

		// Middle: canvas and contextual hints
		auto *mid = new QVBoxLayout;
		mid->addWidget(canvas, 1);
		mid->addWidget(help);

		// Right: inspector tabs
		auto *toggles = new QHBoxLayout;
		toggles->addWidget(lock);
		toggles->addWidget(mirror);
		toggles->addStretch();
		auto *lf = new QFormLayout(layoutTab);
		lf->addRow("Group", group);
		lf->addRow("Column widths", cols);
		lf->addRow("Row heights", rows);
		lf->addRow("Gap", sliderRow(gap, 0, 400, " px"));
		lf->addRow("Margin", sliderRow(margin, 0, 800, " px"));
		lf->addRow(toggles);
		lf->addRow("Safe zone", safe);
		lf->addRow(safeFit);
		auto *del = new QPushButton("Remove Slot");
		auto *sf = new QFormLayout(slotBox);
		sf->setContentsMargins({});
		sf->addRow("Fit", fit);
		sf->addRow("Locked aspect", aspect);
		sf->addRow("Order", order);
		sf->addRow(del);
		auto *sl = new QVBoxLayout(slotTab);
		sl->addWidget(slotHint);
		sl->addWidget(slotBox);
		sl->addStretch();
		tabs->addTab(layoutTab, "Layout");
		tabs->addTab(slotTab, "Slot");
		tabs->setFixedWidth(320);

		auto *body = new QHBoxLayout;
		body->addWidget(left);
		body->addLayout(mid, 1);
		body->addWidget(tabs);
		auto *root = new QVBoxLayout(this);
		root->addLayout(top);
		root->addLayout(body, 1);

		connect(list, &QListWidget::currentItemChanged, this, [this] {
			if (!loading)
				loadLayout();
		});
		connect(filter, &QLineEdit::textChanged, this, [this](const QString &t) {
			list->filter = t;
			list->fill(list->current());
			loadLayout();
		});
		canvas->selected = [this] {
			loadSlot();
			tabs->setCurrentWidget(canvas->sel >= 0 ? slotTab : layoutTab);
		};
		canvas->changed = [this] {
			commit(), loadSlot();
		};

		connect(name, &QLineEdit::textEdited, this, edit([this](Layout &L) { L.name = name->text(); }));
		auto setGroup = [this] {
			const QString g = group->currentText().trimmed();
			if (Layout *L = cur(); L && !loading && !g.isEmpty() && g != L->group) {
				L->group = g;
				reload(list->current());
			}
		};
		connect(group->lineEdit(), &QLineEdit::editingFinished, this, setGroup);
		connect(group, &QComboBox::textActivated, this, setGroup);
		connect(&mode->group, &QButtonGroup::idClicked, this, edit([this](Layout &L) {
			mode->index() ? L.toFree() : L.toGrid();
			loadLayout();
		}));
		auto weights = edit([this](Layout &L) {
			L.cols = parseWeights(cols->text());
			L.rows = parseWeights(rows->text());
			L.slots.removeIf([&](const Slot &s) {
				return s.r.right() > L.cols.size() + 1e-9 || s.r.bottom() > L.rows.size() + 1e-9;
			});
			loadSlot();
		});
		connect(cols, &QLineEdit::textEdited, this, weights);
		connect(rows, &QLineEdit::textEdited, this, weights);
		connect(gap, &QSlider::valueChanged, this, edit([this](Layout &L) { L.gap = gap->value(); }));
		connect(margin, &QSlider::valueChanged, this, edit([this](Layout &L) { L.margin = margin->value(); }));
		connect(lock, &QToolButton::toggled, this,
			edit([this](Layout &L) { L.aspectLock = lock->isChecked(); }));
		connect(mirror, &QToolButton::toggled, this,
			edit([this](Layout &L) { L.mirror = mirror->isChecked(); }));
		connect(&orient->group, &QButtonGroup::idClicked, this,
			edit([this](Layout &L) { L.orient = orient->index(); }));
		connect(safe, &QComboBox::currentIndexChanged, this,
			edit([this](Layout &L) { L.safeZone = safe->currentIndex(); }));
		connect(safeFit, &QCheckBox::toggled, this,
			edit([this](Layout &L) { L.safeFit = safeFit->isChecked(); }));
		connect(&fit->group, &QButtonGroup::idClicked, this,
			editSlot([this](Layout &, Slot &s) { s.fit = fit->index(); }));
		connect(aspect, &QComboBox::currentIndexChanged, this,
			editSlot([this](Layout &, Slot &s) { s.aspect = aspect->currentData().toDouble(); }));
		connect(order, &QSpinBox::valueChanged, this, editSlot([this](Layout &L, Slot &) {
				L.slots.move(canvas->sel, order->value() - 1);
				canvas->sel = order->value() - 1;
				tabs->setTabText(1, QString("Slot %1").arg(canvas->sel + 1));
			}));
		connect(del, &QPushButton::clicked, this, editSlot([this](Layout &L, Slot &) {
				L.slots.removeAt(canvas->sel);
				canvas->select(-1);
			}));

		connect(newBtn, &QPushButton::clicked, this, [this] {
			Layout L;
			L.name = "New Layout";
			L.cols = {1, 1};
			layouts() += L;
			reload(int(layouts().size()) - 1);
		});
		connect(dup, &QToolButton::clicked, this, [this] {
			if (Layout *L = cur()) {
				Layout copy = *L;
				copy.name += " (copy)";
				layouts() += copy;
				reload(int(layouts().size()) - 1);
			}
		});
		connect(menu->addAction("Import Layouts…"), &QAction::triggered, this, [this] {
			const QString f =
				QFileDialog::getOpenFileName(this, "Import Layouts", {}, "Griddy layouts (*.json)");
			const auto imported = readLayoutFile(f);
			layouts() += imported;
			if (!imported.isEmpty())
				reload(int(layouts().size()) - 1);
		});
		connect(menu->addAction("Export Layouts…"), &QAction::triggered, this, [this] {
			const QString f = QFileDialog::getSaveFileName(this, "Export Layouts", "griddy-layouts.json",
								       "Griddy layouts (*.json)");
			if (!f.isEmpty() && !writeLayoutFile(f, layouts()))
				QMessageBox::warning(this, "Griddy", "Could not write " + f);
		});
		menu->addSeparator();
		connect(menu->addAction("Delete Layout"), &QAction::triggered, this, [this] {
			if (const int i = list->current(); i >= 0) {
				layouts().removeAt(i);
				reload(std::min(i, int(layouts().size()) - 1));
			}
		});
		connect(menu->addAction("Reset to Built-in Presets…"), &QAction::triggered, this, [this] {
			if (QMessageBox::question(this, "Griddy", "Replace all layouts with the built-in presets?") ==
			    QMessageBox::Yes) {
				layouts() = builtinLayouts();
				reload(0);
			}
		});
	}

	// Refills the list and selects layout `index`.
	void reload(int index)
	{
		loading = true;
		list->fill(index);
		loading = false;
		loadLayout();
		commit();
	}
};

/* ---------- Dock ---------- */

struct ItemInfo {
	int64_t id;
	QString name;
	bool selected;
};

// Main canvas: preview scene in Studio Mode, else program. Other canvases: the scene on output channel 0.
static obs_source_t *targetScene()
{
	obs_canvas_t *c = targetCanvas();
	obs_source_t *s = nullptr;
	if (obs_canvas_get_flags(c) & MAIN) {
		s = obs_frontend_preview_program_mode_active() ? obs_frontend_get_current_preview_scene()
							       : obs_frontend_get_current_scene();
	} else if ((s = obs_canvas_get_channel(c, 0)) && obs_source_get_type(s) == OBS_SOURCE_TYPE_TRANSITION) {
		obs_source_t *t = s;
		s = obs_transition_get_active_source(t);
		obs_source_release(t);
	}
	obs_canvas_release(c);
	if (s && !obs_scene_from_source(s)) {
		obs_source_release(s);
		s = nullptr;
	}
	return s;
}

// Top-level items of the target scene, top of the source list first.
static QVector<ItemInfo> sceneItems()
{
	QVector<ItemInfo> v;
	if (obs_source_t *src = targetScene()) {
		obs_scene_enum_items(
			obs_scene_from_source(src),
			[](obs_scene_t *, obs_sceneitem_t *it, void *d) {
				static_cast<QVector<ItemInfo> *>(d)->prepend(
					{obs_sceneitem_get_id(it), obs_source_get_name(obs_sceneitem_get_source(it)),
					 obs_sceneitem_selected(it)});
				return true;
			},
			&v);
		obs_source_release(src);
	}
	return v;
}

// The selected layout drawn on the target canvas, with each slot's source name.
class Preview : public QWidget {
public:
	int index = -1;
	QStringList names;
	Preview()
	{
		QSizePolicy sp(QSizePolicy::Expanding, QSizePolicy::Preferred);
		sp.setHeightForWidth(true);
		setSizePolicy(sp);
	}
	const Layout *layout() const { return index >= 0 && index < layouts().size() ? &layouts()[index] : nullptr; }
	bool hasHeightForWidth() const override { return true; }
	int heightForWidth(int w) const override
	{
		const QSizeF c = layout() ? layoutSize(*layout()) : QSizeF(16, 9);
		return std::min(int(w * c.height() / c.width()), 240);
	}
	QSize sizeHint() const override { return {240, heightForWidth(240)}; }

protected:
	void paintEvent(QPaintEvent *) override
	{
		if (const Layout *L = layout()) {
			QPainter p(this);
			const View v(rect(), layoutSize(*L));
			p.fillRect(v.canvas, QColor(30, 30, 36));
			drawSlots(p, v, computeOnCanvas(*L), -1, false, names);
			drawSafeZone(p, v, *L);
		}
	}
};

class GriddyDock : public QWidget {
	LayoutList *list = new LayoutList;
	Segmented *canvasSeg = new Segmented;
	QStringList canvasNames; // per canvas button, empty = main
	QLineEdit *search = new QLineEdit;
	QLabel *hint = new QLabel, *title = new QLabel, *status = new QLabel;
	Preview *preview = new Preview;
	QComboBox *safe = safeZoneBox();
	QSlider *gap = nullptr, *margin = nullptr, *pct = nullptr;
	QToolButton *lock = toggle("Aspect lock", "Keep each source's aspect ratio; the group is centered"),
		    *mirror = toggle("Mirror", "Flip the layout left/right");
	QCheckBox *safeFit = new QCheckBox("Keep inside");
	QFormLayout *slotForm = new QFormLayout, *params = new QFormLayout;
	QWidget *pctRow = nullptr;
	QPushButton *apply = new QPushButton("Apply");
	QVector<QComboBox *> pickers;
	int selCount = -1;
	bool loading = false;

	Layout *cur()
	{
		const int i = list->current();
		return i >= 0 && i < layouts().size() ? &layouts()[i] : nullptr;
	}
	std::function<void()> edit(std::function<void(Layout &)> fn)
	{
		return [this, fn] {
			if (Layout *L = cur(); L && !loading) {
				fn(*L);
				saveLayouts();
				list->currentItem()->setIcon(layoutIcon(*L));
				loadControls();
			}
		};
	}
	void loadControls()
	{
		Layout *L = cur();
		loading = true;
		title->setText(L ? L->name : "No layout for this canvas");
		preview->index = list->current();
		preview->updateGeometry();
		preview->update();
		if (L) {
			gap->setValue(L->gap);
			margin->setValue(L->margin);
			params->setRowVisible(pctRow, L->mainPct() >= 0);
			pct->setValue(std::max(5, L->mainPct()));
			lock->setChecked(L->aspectLock);
			mirror->setChecked(L->mirror);
			safe->setCurrentIndex(L->safeZone);
			safeFit->setChecked(L->safeFit);
		}
		loading = false;
	}
	void setStatus(const QString &text, bool ok = false)
	{
		status->setStyleSheet(ok ? "color: #3fc1a5" : "");
		status->setText(text);
	}
	// Keeps the selection banner and the Apply button in step with OBS.
	void updateHint()
	{
		obs_source_t *src = targetScene();
		apply->setText(src ? QString("Apply to “%1”").arg(obs_source_get_name(src)) : "Apply");
		obs_source_release(src);
		const auto items = sceneItems();
		const int n = int(std::count_if(items.begin(), items.end(), [](auto &i) { return i.selected; }));
		if (n == selCount)
			return;
		selCount = n;
		hint->setText(
			n ? QString("<b>%1 source%2 selected in OBS.</b> Click a layout to place them in order, top "
				    "of the Sources list first.")
					.arg(n)
					.arg(n > 1 ? "s" : "")
			  : "<b>No sources selected in OBS.</b> Pick a source for each slot below and press Apply.");
		hint->setStyleSheet(QString("QLabel { border: 1px solid %1; border-radius: 6px; padding: 6px; }")
					    .arg(n ? "palette(highlight)" : "palette(mid)"));
	}

public:
	GriddyDock()
	{
		list->setViewMode(QListView::IconMode);
		list->setResizeMode(QListView::Adjust);
		list->setMovement(QListView::Static);
		list->setWordWrap(true);
		list->setSpacing(2);
		list->setMinimumHeight(110);
		hint->setWordWrap(true);
		status->setWordWrap(true);
		search->setPlaceholderText("Search layouts");
		search->setClearButtonEnabled(true);
		canvasSeg->setToolTip("Canvas to arrange, e.g. a Vertical Canvas. Only matching layouts are listed.");
		safeFit->setToolTip("Keep slots out of the area covered by the app's UI");
		QFont bold = title->font();
		bold.setBold(true);
		title->setFont(bold);

		auto *refresh = new QToolButton, *editBtn = new QToolButton, *rotate = new QToolButton,
		     *adjust = new QToolButton;
		refresh->setIcon(style()->standardIcon(QStyle::SP_BrowserReload));
		refresh->setToolTip("Reload the canvases and the scene's sources");
		editBtn->setText("Edit Layout…");
		editBtn->setAutoRaise(true);
		rotate->setText("Rotate");
		rotate->setToolTip("Move each source to the next slot and apply");
		apply->setToolTip("Arrange the chosen sources into the layout");
		apply->setStyleSheet("QPushButton { background: palette(highlight); color: palette(highlighted-text); "
				     "font-weight: bold; padding: 6px; border-radius: 4px; }");
		adjust->setText("Adjust this layout");
		adjust->setCheckable(true);
		adjust->setAutoRaise(true);
		adjust->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);

		params->setContentsMargins({});
		params->addRow("Gap", sliderRow(gap, 0, 400, " px"));
		params->addRow("Margin", sliderRow(margin, 0, 800, " px"));
		params->addRow("Main size", pctRow = sliderRow(pct, 5, 95, " %"));
		auto *toggles = new QHBoxLayout;
		toggles->addWidget(lock);
		toggles->addWidget(mirror);
		toggles->addStretch();
		params->addRow(toggles);
		auto *safeRow = new QHBoxLayout;
		safeRow->addWidget(safe, 1);
		safeRow->addWidget(safeFit);
		params->addRow("Safe zone", safeRow);
		auto *settings = new QWidget;
		settings->setLayout(params);

		auto *head = new QHBoxLayout;
		head->addWidget(title, 1);
		head->addWidget(editBtn);
		auto *card = new QFrame;
		card->setFrameShape(QFrame::StyledPanel);
		card->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Maximum);
		auto *cl = new QVBoxLayout(card);
		cl->addLayout(head);
		cl->addWidget(preview);
		cl->addLayout(slotForm);
		cl->addWidget(adjust);
		cl->addWidget(settings);

		auto *searchRow = new QHBoxLayout;
		searchRow->addWidget(search, 1);
		searchRow->addWidget(refresh);
		auto *actions = new QHBoxLayout;
		actions->addWidget(apply, 1);
		actions->addWidget(rotate);

		auto *root = new QVBoxLayout(this);
		root->addWidget(canvasSeg);
		root->addWidget(hint);
		root->addLayout(searchRow);
		root->addWidget(list, 1);
		root->addWidget(card);
		root->addLayout(actions);
		root->addWidget(status);

		connect(adjust, &QToolButton::toggled, this, [adjust, settings](bool on) {
			settings->setVisible(on);
			adjust->setArrowType(on ? Qt::DownArrow : Qt::RightArrow);
			config_set_bool(obs_frontend_get_user_config(), "Griddy", "AdjustHidden", !on);
		});
		adjust->setChecked(!config_get_bool(obs_frontend_get_user_config(), "Griddy", "AdjustHidden"));
		Q_EMIT adjust->toggled(adjust->isChecked());

		connect(list, &QListWidget::currentItemChanged, this, [this] {
			loadControls();
			refreshPickers();
		});
		// One click: selected sources go straight into the layout; otherwise pick them below.
		connect(list, &QListWidget::itemClicked, this, [this](QListWidgetItem *it) {
			const auto items = sceneItems();
			const auto n = std::count_if(items.begin(), items.end(), [](auto &i) { return i.selected; });
			if (list->layoutAt(it) < 0 || !n)
				return;
			refreshPickers(true);
			applyLayout();
			if (n > pickers.size())
				status->setText(status->text() + QString(" %1 extra selected source(s) left untouched.")
									 .arg(n - pickers.size()));
		});
		connect(search, &QLineEdit::textChanged, this, [this](const QString &t) {
			list->filter = t;
			refreshList();
		});
		connect(gap, &QSlider::valueChanged, this, edit([this](Layout &L) { L.gap = gap->value(); }));
		connect(margin, &QSlider::valueChanged, this, edit([this](Layout &L) { L.margin = margin->value(); }));
		connect(pct, &QSlider::valueChanged, this, edit([this](Layout &L) { L.setMainPct(pct->value()); }));
		connect(lock, &QToolButton::toggled, this,
			edit([this](Layout &L) { L.aspectLock = lock->isChecked(); }));
		connect(mirror, &QToolButton::toggled, this,
			edit([this](Layout &L) { L.mirror = mirror->isChecked(); }));
		connect(safe, &QComboBox::currentIndexChanged, this,
			edit([this](Layout &L) { L.safeZone = safe->currentIndex(); }));
		connect(safeFit, &QCheckBox::toggled, this,
			edit([this](Layout &L) { L.safeFit = safeFit->isChecked(); }));
		connect(&canvasSeg->group, &QButtonGroup::idClicked, this, [this](int i) {
			g_canvasName = canvasNames.value(i);
			config_set_string(obs_frontend_get_user_config(), "Griddy", "Canvas",
					  g_canvasName.toUtf8().constData());
			selCount = -1;
			refreshList();
		});
		connect(apply, &QPushButton::clicked, this, [this] { applyLayout(); });
		connect(refresh, &QToolButton::clicked, this, [this] {
			refreshCanvases();
			refreshPickers();
		});
		connect(editBtn, &QToolButton::clicked, this, [this] { openEditor(list->current()); });
		connect(rotate, &QToolButton::clicked, this, [this] {
			QVector<qint64> ids;
			for (auto *c : pickers)
				ids += c->currentData().toLongLong();
			if (ids.size() < 2)
				return;
			std::rotate(ids.begin(), ids.begin() + 1, ids.end());
			for (int i = 0; i < ids.size(); i++)
				pickers[i]->setCurrentIndex(std::max(0, pickers[i]->findData(ids[i])));
			applyLayout();
		});
		auto *timer = new QTimer(this);
		connect(timer, &QTimer::timeout, this, [this] {
			if (isVisible())
				updateHint();
		});
		timer->start(500);

		g_canvasName = QString::fromUtf8(config_get_string(obs_frontend_get_user_config(), "Griddy", "Canvas"));
		refreshCanvases();
		updateHint();
	}

	static void openEditor(int index);

	// Main canvas plus extra canvases from plugins such as Vertical Canvas. A saved canvas that isn't
	// loaded yet stays the target; until it appears, lookups fall back to the main canvas.
	void refreshCanvases()
	{
		canvasNames = {QString()};
		obs_frontend_canvas_list l = {};
		obs_frontend_get_canvases(&l);
		for (size_t i = 0; i < l.canvases.num; i++)
			if (!(obs_canvas_get_flags(l.canvases.array[i]) & MAIN))
				canvasNames += obs_canvas_get_name(l.canvases.array[i]);
		obs_frontend_canvas_list_free(&l);
		QStringList labels;
		for (const auto &n : canvasNames) {
			const QSizeF c = canvasSize(n);
			labels += QString("%1  %2×%3").arg(n.isEmpty() ? "Main" : n).arg(c.width()).arg(c.height());
		}
		canvasSeg->setItems(labels);
		canvasSeg->setIndex(std::max(0, int(canvasNames.indexOf(g_canvasName))));
		refreshList();
	}

	// Lists the layouts, hiding those made for the other orientation than the target canvas.
	void refreshList()
	{
		const QSizeF c = canvasSize();
		list->setIconSize(c.scaled(96, 96, Qt::KeepAspectRatio).toSize());
		list->show = [c](const Layout &L) {
			return L.fits(c.width(), c.height());
		};
		list->fill(list->current());
		loadControls();
		refreshPickers();
	}

	// Rebuilds the slot pickers; keeps previous choices unless filling from the OBS selection.
	void refreshPickers(bool fromSelection = false)
	{
		QVector<qint64> want;
		const auto items = sceneItems();
		if (fromSelection)
			for (const auto &it : items)
				if (it.selected)
					want += it.id;
		if (!fromSelection)
			for (auto *c : pickers)
				want += c->currentData().toLongLong();
		while (slotForm->rowCount())
			slotForm->removeRow(0);
		pickers.clear();
		const Layout *L = cur();
		for (int i = 0; L && i < L->slots.size(); i++) {
			auto *badge = new QLabel(QString::number(i + 1));
			badge->setAlignment(Qt::AlignCenter);
			badge->setFixedSize(22, 22);
			badge->setToolTip(i ? QString("Slot %1").arg(i + 1) : "Slot 1 (main)");
			badge->setStyleSheet(
				QString("background: %1; color: #111; border-radius: 4px; font-weight: bold;")
					.arg(kColors[i % 6].name()));
			auto *c = new QComboBox;
			c->addItem("(none)", qint64(-1));
			for (const auto &it : items)
				c->addItem(it.name, qint64(it.id));
			c->setCurrentIndex(std::max(0, c->findData(i < want.size() ? want[i] : -1)));
			connect(c, &QComboBox::currentIndexChanged, this, [this] { updateNames(); });
			slotForm->addRow(badge, c);
			pickers += c;
		}
		updateNames();
	}
	void updateNames()
	{
		preview->names.clear();
		for (auto *c : pickers)
			preview->names += c->currentIndex() > 0 ? c->currentText() : QString();
		preview->update();
	}

	void applyLayout()
	{
		const Layout *L = cur();
		obs_source_t *src = targetScene();
		if (!src)
			return setStatus("No active scene on the target canvas.");
		if (!L)
			return obs_source_release(src);
		obs_scene_t *scene = obs_scene_from_source(src);
		const auto rects = computeOnCanvas(*L);
		obs_data_t *before = obs_scene_save_transform_states(scene, true);
		int n = 0;
		for (int i = 0; i < pickers.size() && i < rects.size(); i++) {
			obs_sceneitem_t *it =
				obs_scene_find_sceneitem_by_id(scene, pickers[i]->currentData().toLongLong());
			if (!it || rects[i].isEmpty())
				continue;
			const int fit = L->slots[i].fit;
			obs_transform_info t = {};
			vec2_set(&t.pos, rects[i].x(), rects[i].y());
			vec2_set(&t.scale, 1, 1);
			vec2_set(&t.bounds, rects[i].width(), rects[i].height());
			t.alignment = OBS_ALIGN_LEFT | OBS_ALIGN_TOP;
			t.bounds_alignment = OBS_ALIGN_CENTER;
			t.bounds_type = fit == FitCrop     ? OBS_BOUNDS_SCALE_OUTER
					: fit == FitInside ? OBS_BOUNDS_SCALE_INNER
							   : OBS_BOUNDS_STRETCH;
			t.crop_to_bounds = fit == FitCrop;
			obs_sceneitem_set_info2(it, &t);
			n++;
		}
		obs_data_t *after = obs_scene_save_transform_states(scene, true);
		auto load = [](const char *d) {
			obs_scene_load_transform_states(d);
		};
		if (n)
			obs_frontend_add_undo_redo_action(
				("Apply Griddy layout '" + L->name + "'").toUtf8().constData(), load, load,
				obs_data_get_json(before), obs_data_get_json(after), false);
		obs_data_release(before);
		obs_data_release(after);
		setStatus(n ? QString("Applied %1 of %2 slot(s) in '%3'. Ctrl+Z undoes it.")
					  .arg(n)
					  .arg(L->slots.size())
					  .arg(obs_source_get_name(src))
			    : "Nothing applied. Pick a source for at least one slot.",
			  n > 0);
		obs_source_release(src);
	}
};

/* ---------- Module ---------- */

static QPointer<GriddyDock> g_dock;
static QPointer<EditorDialog> g_editor;

void GriddyDock::openEditor(int index)
{
	if (!g_editor)
		g_editor = new EditorDialog(static_cast<QWidget *>(obs_frontend_get_main_window()));
	g_editor->reload(std::max(0, index));
	g_editor->show();
	g_editor->raise();
	g_editor->activateWindow();
}

static void onFrontendEvent(enum obs_frontend_event e, void *)
{
	if (!g_dock)
		return;
	if (e == OBS_FRONTEND_EVENT_PROFILE_CHANGED)
		g_dock->refreshList();
	else if (e == OBS_FRONTEND_EVENT_CANVAS_ADDED || e == OBS_FRONTEND_EVENT_CANVAS_REMOVED ||
		 e == OBS_FRONTEND_EVENT_FINISHED_LOADING)
		g_dock->refreshCanvases();
	else if (e == OBS_FRONTEND_EVENT_SCENE_CHANGED || e == OBS_FRONTEND_EVENT_PREVIEW_SCENE_CHANGED ||
		 e == OBS_FRONTEND_EVENT_STUDIO_MODE_ENABLED || e == OBS_FRONTEND_EVENT_STUDIO_MODE_DISABLED ||
		 e == OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGED || e == OBS_FRONTEND_EVENT_FINISHED_LOADING)
		g_dock->refreshPickers();
}

bool obs_module_load(void)
{
	g_dock = new GriddyDock;
	obs_frontend_add_dock_by_id("griddy", obs_module_text("Griddy"), g_dock);
	g_layoutsChanged = [] {
		if (g_dock)
			g_dock->refreshList();
	};
	obs_frontend_add_tools_menu_item(
		obs_module_text("LayoutEditor"), [](void *) { GriddyDock::openEditor(0); }, nullptr);
	obs_frontend_add_event_callback(onFrontendEvent, nullptr);
	obs_log(LOG_INFO, "plugin loaded successfully (version %s)", PLUGIN_VERSION);
	return true;
}

void obs_module_unload(void)
{
	obs_frontend_remove_event_callback(onFrontendEvent, nullptr);
	g_layoutsChanged = nullptr;
}
