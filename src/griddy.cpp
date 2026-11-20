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
#include <functional>
#include <numeric>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE(PLUGIN_NAME, "en-US")

static std::function<void()> g_layoutsChanged; // editor -> dock

static QSizeF canvasSize()
{
	obs_video_info o;
	return obs_get_video_info(&o) ? QSizeF(o.base_width, o.base_height) : QSizeF(1920, 1080);
}

// Maps canvas pixels into a widget area, keeping the canvas aspect ratio.
struct View {
	QRectF canvas;
	double s;
	explicit View(const QRectF &area)
	{
		const QSizeF c = canvasSize();
		canvas = QRectF(QPointF(), c.scaled(area.size(), Qt::KeepAspectRatio));
		canvas.moveCenter(area.center());
		s = canvas.width() / c.width();
	}
	QRectF map(const QRectF &r) const { return {canvas.topLeft() + r.topLeft() * s, r.size() * s}; }
};

static QVector<QRectF> computeOnCanvas(const Layout &L)
{
	return L.compute(canvasSize().width(), canvasSize().height());
}

static void drawSlots(QPainter &p, const View &v, const QVector<QRectF> &rects, int sel = -1, bool outline = false)
{
	static const QColor colors[] = {0x4c8bf5, 0xf59e0b, 0x10b981, 0xec4899, 0x8b5cf6, 0x06b6d4};
	p.setRenderHint(QPainter::Antialiasing);
	for (int i = 0; i < rects.size(); i++) {
		const QRectF q = v.map(rects[i]);
		if (outline) {
			p.setPen(QPen(Qt::white, 1, Qt::DashLine));
			p.setBrush(Qt::NoBrush);
			p.drawRect(q);
			continue;
		}
		QColor c = colors[i % 6];
		p.setPen(QPen(i == sel ? Qt::white : c.lighter(130), i == sel ? 2.5 : 1));
		c.setAlpha(170);
		p.setBrush(c);
		p.drawRect(q);
		QFont f = p.font();
		f.setPixelSize(qBound(8, int(q.height() * 0.35), 32));
		f.setBold(true);
		p.setFont(f);
		p.setPen(Qt::white);
		p.drawText(q, Qt::AlignCenter, QString::number(i + 1));
	}
}

static QIcon layoutIcon(const Layout &L)
{
	QPixmap pm(128, 72);
	pm.fill(Qt::transparent);
	{
		QPainter p(&pm);
		const View v(pm.rect());
		p.fillRect(v.canvas, QColor(30, 30, 36));
		drawSlots(p, v, computeOnCanvas(L));
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

	// Editing works on the plain layout; aspect-locked results are drawn dashed on top.
	Layout base() const
	{
		Layout b = *L;
		b.aspectLock = b.mirror = false;
		return b;
	}
	QRectF content() const
	{
		const View v(rect());
		const double m = L->margin * v.s;
		return v.canvas.adjusted(m, m, -m, -m);
	}
	QVector<QRectF> shown() const
	{
		const View v(rect());
		QVector<QRectF> out;
		for (const auto &r : computeOnCanvas(base()))
			out += v.map(r);
		return out;
	}
	int slotAt(QPointF p) const
	{
		const auto s = shown();
		for (int i = s.size() - 1; i >= 0; i--)
			if (s[i].contains(p))
				return i;
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
		const View v(rect());
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
		if (L->aspectLock) {
			Layout f = *L;
			f.mirror = false;
			drawSlots(p, v, computeOnCanvas(f), -1, true);
		}
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
			select(L->slots.size() - 1);
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
			for (int i = s.size() - 1; i >= 0; i--)
				if (s[i].r.toRect().intersects(span))
					s.removeAt(i);
			s += Slot{QRectF(span), FitCrop, 0};
			select(s.size() - 1);
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

class EditorDialog : public QDialog {
	QListWidget *list = new QListWidget;
	LayoutCanvas *canvas = new LayoutCanvas;
	QLineEdit *name = new QLineEdit, *cols = new QLineEdit, *rows = new QLineEdit;
	QComboBox *mode = new QComboBox, *fit = new QComboBox, *aspect = new QComboBox;
	QSpinBox *gap = spin(400), *margin = spin(800), *order = spin(1, "");
	QCheckBox *lock = new QCheckBox("Aspect lock"), *mirror = new QCheckBox("Mirror (flip left/right)");
	QGroupBox *layoutBox = new QGroupBox("Layout"), *slotBox = new QGroupBox("Selected slot");
	bool loading = false;

	Layout *cur()
	{
		const int i = list->currentRow();
		return i >= 0 && i < layouts().size() ? &layouts()[i] : nullptr;
	}
	void commit()
	{
		saveLayouts();
		if (auto *it = list->currentItem(); it && cur())
			it->setText(cur()->name);
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
		layoutBox->setEnabled(L);
		if (L) {
			name->setText(L->name);
			mode->setCurrentIndex(L->grid ? 0 : 1);
			cols->setText(weightsText(L->cols));
			rows->setText(weightsText(L->rows));
			cols->setEnabled(L->grid);
			rows->setEnabled(L->grid);
			gap->setValue(L->gap);
			margin->setValue(L->margin);
			lock->setChecked(L->aspectLock);
			mirror->setChecked(L->mirror);
		}
		loading = false;
		loadSlot();
	}
	void loadSlot()
	{
		Layout *L = cur();
		const int i = canvas->sel;
		const bool ok = L && i >= 0 && i < L->slots.size();
		slotBox->setEnabled(ok);
		loading = true;
		if (ok) {
			fit->setCurrentIndex(L->slots[i].fit);
			aspect->setCurrentIndex(std::max(0, aspect->findData(L->slots[i].aspect)));
			order->setRange(1, L->slots.size());
			order->setValue(i + 1);
		}
		loading = false;
		canvas->update();
	}

public:
	explicit EditorDialog(QWidget *parent) : QDialog(parent)
	{
		setWindowTitle("Griddy Layout Editor");
		list->setIconSize(QSize(64, 36));
		resize(1100, 560);
		mode->addItems({"Grid", "Free"});
		fit->addItems({"Crop to fill", "Fit inside", "Stretch"});
		for (const auto &[n, a] : kAspects)
			aspect->addItem(n, a);
		cols->setPlaceholderText("e.g. 2, 1");
		rows->setPlaceholderText("e.g. 1, 1");

		auto *lf = new QFormLayout(layoutBox);
		lf->addRow("Name", name);
		lf->addRow("Mode", mode);
		lf->addRow("Column weights", cols);
		lf->addRow("Row weights", rows);
		lf->addRow("Gap", gap);
		lf->addRow("Margin", margin);
		lf->addRow(lock);
		lf->addRow(mirror);

		auto *del = new QPushButton("Remove slot");
		auto *sf = new QFormLayout(slotBox);
		sf->addRow("Fit", fit);
		sf->addRow("Aspect", aspect);
		sf->addRow("Order", order);
		sf->addRow(del);

		auto *help = new QLabel("<b>Grid:</b> click a cell to add a slot, drag across cells to merge.<br>"
					"<b>Free:</b> drag to draw, drag a slot to move, drag its corner to resize.<br>"
					"Del removes the selected slot. Slot 1 is the main slot. "
					"Dashed outlines show the result with aspect lock.");
		help->setWordWrap(true);

		auto *btns = new QGridLayout;
		const QStringList names{"New", "Duplicate", "Delete", "Import…", "Export…", "Reset Defaults"};
		QVector<QPushButton *> b;
		for (int i = 0; i < names.size(); i++)
			btns->addWidget(b.emplace_back(new QPushButton(names[i])), i / 2, i % 2);

		auto *left = new QVBoxLayout;
		left->addWidget(list);
		left->addLayout(btns);
		auto *mid = new QVBoxLayout;
		mid->addWidget(canvas, 1);
		mid->addWidget(help);
		auto *right = new QVBoxLayout;
		right->addWidget(layoutBox);
		right->addWidget(slotBox);
		right->addStretch();
		auto *root = new QHBoxLayout(this);
		root->addLayout(left, 0);
		root->addLayout(mid, 1);
		root->addLayout(right, 0);

		connect(list, &QListWidget::currentRowChanged, this, [this] { loadLayout(); });
		canvas->selected = [this] {
			loadSlot();
		};
		canvas->changed = [this] {
			commit(), loadSlot();
		};

		connect(name, &QLineEdit::textEdited, this, edit([this](Layout &L) { L.name = name->text(); }));
		connect(mode, &QComboBox::currentIndexChanged, this, edit([this](Layout &L) {
				mode->currentIndex() ? L.toFree() : L.toGrid();
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
		connect(gap, &QSpinBox::valueChanged, this, edit([this](Layout &L) { L.gap = gap->value(); }));
		connect(margin, &QSpinBox::valueChanged, this, edit([this](Layout &L) { L.margin = margin->value(); }));
		connect(lock, &QCheckBox::toggled, this, edit([this](Layout &L) { L.aspectLock = lock->isChecked(); }));
		connect(mirror, &QCheckBox::toggled, this, edit([this](Layout &L) { L.mirror = mirror->isChecked(); }));
		connect(fit, &QComboBox::currentIndexChanged, this,
			editSlot([this](Layout &, Slot &s) { s.fit = fit->currentIndex(); }));
		connect(aspect, &QComboBox::currentIndexChanged, this,
			editSlot([this](Layout &, Slot &s) { s.aspect = aspect->currentData().toDouble(); }));
		connect(order, &QSpinBox::valueChanged, this, editSlot([this](Layout &L, Slot &) {
				L.slots.move(canvas->sel, order->value() - 1);
				canvas->sel = order->value() - 1;
			}));
		connect(del, &QPushButton::clicked, this, editSlot([this](Layout &L, Slot &) {
				L.slots.removeAt(canvas->sel);
				canvas->select(-1);
			}));

		connect(b[0], &QPushButton::clicked, this, [this] {
			Layout L;
			L.name = "New Layout";
			L.cols = {1, 1};
			layouts() += L;
			reload(layouts().size() - 1);
		});
		connect(b[1], &QPushButton::clicked, this, [this] {
			if (Layout *L = cur()) {
				Layout copy = *L;
				copy.name += " (copy)";
				layouts() += copy;
				reload(layouts().size() - 1);
			}
		});
		connect(b[2], &QPushButton::clicked, this, [this] {
			if (cur()) {
				const int i = list->currentRow();
				layouts().removeAt(i);
				reload(std::min(i, int(layouts().size()) - 1));
			}
		});
		connect(b[3], &QPushButton::clicked, this, [this] {
			const QString f =
				QFileDialog::getOpenFileName(this, "Import Layouts", {}, "Griddy layouts (*.json)");
			const auto imported = readLayoutFile(f);
			layouts() += imported;
			if (!imported.isEmpty())
				reload(layouts().size() - 1);
		});
		connect(b[4], &QPushButton::clicked, this, [this] {
			const QString f = QFileDialog::getSaveFileName(this, "Export Layouts", "griddy-layouts.json",
								       "Griddy layouts (*.json)");
			if (!f.isEmpty() && !writeLayoutFile(f, layouts()))
				QMessageBox::warning(this, "Griddy", "Could not write " + f);
		});
		connect(b[5], &QPushButton::clicked, this, [this] {
			if (QMessageBox::question(this, "Griddy", "Replace all layouts with the built-in presets?") ==
			    QMessageBox::Yes) {
				layouts() = builtinLayouts();
				reload(0);
			}
		});
	}

	void reload(int row)
	{
		loading = true;
		list->clear();
		for (const auto &L : layouts())
			list->addItem(new QListWidgetItem(layoutIcon(L), L.name));
		loading = false;
		list->setCurrentRow(row);
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

static obs_source_t *targetScene()
{
	return obs_frontend_preview_program_mode_active() ? obs_frontend_get_current_preview_scene()
							  : obs_frontend_get_current_scene();
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

class GriddyDock : public QWidget {
	QListWidget *list = new QListWidget;
	QSpinBox *gap = spin(400), *margin = spin(800), *pct = spin(95, " %");
	QCheckBox *lock = new QCheckBox("Aspect lock"), *mirror = new QCheckBox("Mirror");
	QFormLayout *slotForm = new QFormLayout;
	QVector<QComboBox *> pickers;
	QLabel *status = new QLabel;
	bool loading = false;

	Layout *cur()
	{
		const int i = list->currentRow();
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
		if (L) {
			gap->setValue(L->gap);
			margin->setValue(L->margin);
			pct->setEnabled(L->mainPct() >= 0);
			pct->setValue(std::max(0, L->mainPct()));
			lock->setChecked(L->aspectLock);
			mirror->setChecked(L->mirror);
		}
		loading = false;
	}

public:
	GriddyDock()
	{
		list->setViewMode(QListView::IconMode);
		list->setIconSize(QSize(96, 54));
		list->setResizeMode(QListView::Adjust);
		list->setMovement(QListView::Static);
		list->setWordWrap(true);
		list->setMinimumHeight(110);
		pct->setRange(5, 95);
		status->setWordWrap(true);

		auto *params = new QFormLayout;
		params->addRow("Gap", gap);
		params->addRow("Margin", margin);
		params->addRow("Main size", pct);
		auto *checks = new QHBoxLayout;
		checks->addWidget(lock);
		checks->addWidget(mirror);
		params->addRow(checks);

		auto *slots = new QGroupBox("Slots");
		slots->setLayout(slotForm);

		auto *apply = new QPushButton("Apply"), *rotate = new QPushButton("Rotate"),
		     *refresh = new QPushButton("Refresh"), *editBtn = new QPushButton("Edit Layouts…");
		apply->setToolTip("Arrange the chosen sources into the layout");
		rotate->setToolTip("Shift each source to the next slot and apply");
		refresh->setToolTip("Reload the scene's sources");
		auto *btns = new QGridLayout;
		btns->addWidget(apply, 0, 0);
		btns->addWidget(rotate, 0, 1);
		btns->addWidget(refresh, 1, 0);
		btns->addWidget(editBtn, 1, 1);

		auto *root = new QVBoxLayout(this);
		root->addWidget(list, 1);
		root->addLayout(params);
		root->addWidget(slots);
		root->addLayout(btns);
		root->addWidget(status);

		connect(list, &QListWidget::currentRowChanged, this, [this] {
			loadControls();
			refreshPickers();
		});
		// One click: selected sources go straight into the layout; otherwise pick them below.
		connect(list, &QListWidget::itemClicked, this, [this] {
			const auto items = sceneItems();
			const auto n = std::count_if(items.begin(), items.end(), [](auto &i) { return i.selected; });
			if (!n)
				return status->setText("Select sources in OBS, or choose them below and press Apply.");
			refreshPickers(true);
			applyLayout();
			if (n > pickers.size())
				status->setText(status->text() + QString(" %1 extra selected source(s) left untouched.")
									 .arg(n - pickers.size()));
		});
		connect(gap, &QSpinBox::valueChanged, this, edit([this](Layout &L) { L.gap = gap->value(); }));
		connect(margin, &QSpinBox::valueChanged, this, edit([this](Layout &L) { L.margin = margin->value(); }));
		connect(pct, &QSpinBox::valueChanged, this, edit([this](Layout &L) { L.setMainPct(pct->value()); }));
		connect(lock, &QCheckBox::toggled, this, edit([this](Layout &L) { L.aspectLock = lock->isChecked(); }));
		connect(mirror, &QCheckBox::toggled, this, edit([this](Layout &L) { L.mirror = mirror->isChecked(); }));
		connect(apply, &QPushButton::clicked, this, [this] { applyLayout(); });
		connect(refresh, &QPushButton::clicked, this, [this] { refreshPickers(); });
		connect(editBtn, &QPushButton::clicked, this, [this] { openEditor(list->currentRow()); });
		connect(rotate, &QPushButton::clicked, this, [this] {
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

		refreshList();
	}

	static void openEditor(int row);

	void refreshList()
	{
		const int row = std::max(0, list->currentRow());
		const QSignalBlocker block(list);
		list->clear();
		for (const auto &L : layouts())
			list->addItem(new QListWidgetItem(layoutIcon(L), L.name));
		list->setCurrentRow(std::min(row, int(layouts().size()) - 1));
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
			auto *c = new QComboBox;
			c->addItem("(none)", qint64(-1));
			for (const auto &it : items)
				c->addItem(it.name, qint64(it.id));
			c->setCurrentIndex(std::max(0, c->findData(i < want.size() ? want[i] : -1)));
			slotForm->addRow(i ? QString::number(i + 1) : "1 (main)", c);
			pickers += c;
		}
	}

	void applyLayout()
	{
		const Layout *L = cur();
		obs_source_t *src = targetScene();
		if (!L || !src)
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
		status->setText(QString("Applied %1 of %2 slot(s) in '%3'.")
					.arg(n)
					.arg(L->slots.size())
					.arg(obs_source_get_name(src)));
		obs_source_release(src);
	}
};

/* ---------- Module ---------- */

static QPointer<GriddyDock> g_dock;
static QPointer<EditorDialog> g_editor;

void GriddyDock::openEditor(int row)
{
	if (!g_editor)
		g_editor = new EditorDialog(static_cast<QWidget *>(obs_frontend_get_main_window()));
	g_editor->reload(std::max(0, row));
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
