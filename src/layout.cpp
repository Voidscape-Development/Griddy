#include "layout.hpp"
#include <obs-module.h>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <cmath>

static double sum(const QVector<double> &v, int a = 0, int n = -1)
{
	double s = 0;
	for (int i = a; i < (n < 0 ? v.size() : a + n); i++)
		s += v[i];
	return s;
}

QVector<QRectF> Layout::compute(double W, double H) const
{
	QVector<QRectF> out;
	const double cw = W - 2 * margin, ch = H - 2 * margin;
	if (cw <= 0 || ch <= 0)
		return out;
	if (grid) {
		const int nc = cols.size(), nr = rows.size();
		const double aw = cw - (nc - 1) * gap, ah = ch - (nr - 1) * gap;
		QVector<double> c = cols, r = rows;
		for (auto &x : c)
			x *= aw / sum(cols);
		for (auto &x : r)
			x *= ah / sum(rows);
		auto span = [&](const QVector<double> &v, int a, int n) {
			return sum(v, a, n) + (n - 1) * gap;
		};
		auto ok = [&](const QRect &q) {
			return q.x() >= 0 && q.y() >= 0 && q.width() > 0 && q.height() > 0 && q.right() < nc &&
			       q.bottom() < nr;
		};
		if (aspectLock) {
			// Column weights set widths; locked slots resize their rows. Later slots first so the main slot wins.
			for (int k = slots.size() - 1; k >= 0; k--) {
				const QRect q = slots[k].r.toRect();
				if (slots[k].aspect <= 0 || !ok(q))
					continue;
				const double want =
					span(c, q.x(), q.width()) / slots[k].aspect - (q.height() - 1) * gap;
				const double have = sum(r, q.y(), q.height());
				for (int i = q.y(); want > 0 && have > 0 && i <= q.bottom(); i++)
					r[i] *= want / have;
			}
			const double k = std::min(1.0, ah / sum(r)); // shrink uniformly when too tall
			for (auto &x : c)
				x *= k;
			for (auto &x : r)
				x *= k;
		}
		const double ox = margin + (cw - span(c, 0, nc)) / 2, oy = margin + (ch - span(r, 0, nr)) / 2;
		for (const Slot &s : slots) {
			const QRect q = s.r.toRect();
			out += ok(q) ? QRectF(ox + sum(c, 0, q.x()) + q.x() * gap, oy + sum(r, 0, q.y()) + q.y() * gap,
					      span(c, q.x(), q.width()), span(r, q.y(), q.height()))
				     : QRectF();
		}
	} else {
		const double e = 1e-3, g = gap / 2.0;
		for (const Slot &s : slots) {
			const QRectF &n = s.r;
			QRectF p(margin + n.x() * cw, margin + n.y() * ch, n.width() * cw, n.height() * ch);
			// Half a gap on every edge that doesn't touch the content border
			p.adjust(n.left() > e ? g : 0, n.top() > e ? g : 0, n.right() < 1 - e ? -g : 0,
				 n.bottom() < 1 - e ? -g : 0);
			if (aspectLock && s.aspect > 0) {
				const QSizeF sz = QSizeF(s.aspect, 1).scaled(p.size(), Qt::KeepAspectRatio);
				p = QRectF(p.center() - QPointF(sz.width(), sz.height()) / 2, sz);
			}
			out += p;
		}
	}
	if (mirror)
		for (auto &p : out)
			p.moveLeft(W - p.right());
	return out;
}

// Weight vector and range of the axis the main slot only partially spans.
static QVector<double> *mainSpan(Layout &L, int &a, int &n)
{
	if (!L.grid || L.slots.isEmpty())
		return nullptr;
	const QRect r = L.slots[0].r.toRect();
	if (r.width() < L.cols.size())
		return a = r.x(), n = r.width(), &L.cols;
	if (r.height() < L.rows.size())
		return a = r.y(), n = r.height(), &L.rows;
	return nullptr;
}

int Layout::mainPct() const
{
	int a, n;
	auto *v = mainSpan(const_cast<Layout &>(*this), a, n);
	return v ? qRound(100 * sum(*v, a, n) / sum(*v)) : -1;
}

void Layout::setMainPct(int pct)
{
	int a, n;
	auto *v = mainSpan(*this, a, n);
	if (!v)
		return;
	const double t = sum(*v), in = sum(*v, a, n), p = qBound(5, pct, 95) / 100.0;
	for (int i = 0; i < v->size(); i++)
		(*v)[i] *= (i >= a && i < a + n) ? p * t / in : (1 - p) * t / (t - in);
}

void Layout::toFree()
{
	Layout t = *this;
	t.gap = t.margin = 0;
	t.aspectLock = t.mirror = false;
	const auto rs = t.compute(1, 1);
	for (int i = 0; i < slots.size(); i++)
		slots[i].r = rs[i];
	grid = false;
}

// Index of the weight boundary closest to fraction f.
static int nearest(const QVector<double> &v, double f)
{
	int best = 0;
	for (int i = 1; i <= v.size(); i++)
		if (std::abs(sum(v, 0, i) / sum(v) - f) < std::abs(sum(v, 0, best) / sum(v) - f))
			best = i;
	return best;
}

void Layout::toGrid()
{
	for (Slot &s : slots) {
		const int x0 = std::min(nearest(cols, s.r.left()), int(cols.size()) - 1);
		const int y0 = std::min(nearest(rows, s.r.top()), int(rows.size()) - 1);
		const int x1 = std::max(nearest(cols, s.r.right()), x0 + 1);
		const int y1 = std::max(nearest(rows, s.r.bottom()), y0 + 1);
		s.r = QRectF(x0, y0, x1 - x0, y1 - y0);
	}
	grid = true;
}

static QJsonArray toArray(const QVector<double> &v)
{
	QJsonArray a;
	for (double d : v)
		a += d;
	return a;
}

static QVector<double> toWeights(const QJsonValue &j)
{
	QVector<double> v;
	for (const auto &x : j.toArray())
		if (x.toDouble() > 0)
			v += x.toDouble();
	return v.isEmpty() ? QVector<double>{1} : v;
}

QJsonObject Layout::toJson() const
{
	QJsonArray s;
	for (const Slot &x : slots)
		s += QJsonObject{{"x", x.r.x()},      {"y", x.r.y()}, {"w", x.r.width()},
				 {"h", x.r.height()}, {"fit", x.fit}, {"aspect", x.aspect}};
	return {{"name", name}, {"grid", grid},     {"cols", toArray(cols)},    {"rows", toArray(rows)},
		{"gap", gap},   {"margin", margin}, {"aspectLock", aspectLock}, {"mirror", mirror},
		{"slots", s}};
}

Layout Layout::fromJson(const QJsonObject &o)
{
	Layout L;
	L.name = o["name"].toString("Layout");
	L.grid = o["grid"].toBool(true);
	L.cols = toWeights(o["cols"]);
	L.rows = toWeights(o["rows"]);
	L.gap = o["gap"].toInt(10);
	L.margin = o["margin"].toInt(0);
	L.aspectLock = o["aspectLock"].toBool(true);
	L.mirror = o["mirror"].toBool(false);
	for (const auto &v : o["slots"].toArray()) {
		const auto j = v.toObject();
		L.slots += {QRectF(j["x"].toDouble(), j["y"].toDouble(), j["w"].toDouble(), j["h"].toDouble()),
			    qBound(0, j["fit"].toInt(), 2), j["aspect"].toDouble()};
	}
	return L;
}

QVector<Layout> builtinLayouts()
{
	auto g = [](const char *n, QVector<double> c, QVector<double> r, QVector<Slot> s, int gap = 20) {
		Layout L;
		L.name = n, L.cols = c, L.rows = r, L.slots = s, L.gap = gap;
		return L;
	};
	auto f = [&](const char *n, QVector<Slot> s, int gap = 20) { // free-mode layout
		Layout L = g(n, {1}, {1}, s, gap);
		L.grid = false;
		return L;
	};
	auto s = [](double x, double y, double w = 1, double h = 1, double a = 16.0 / 9) {
		return Slot{QRectF(x, y, w, h), FitCrop, a};
	};
	QVector<Slot> grid3x3, lShape{s(0, 0, 2, 2)};
	for (int i = 0; i < 9; i++)
		grid3x3 += s(i % 3, i / 3);
	for (QPoint p : {QPoint(2, 0), QPoint(2, 1), QPoint(2, 2), QPoint(1, 2), QPoint(0, 2)})
		lShape += s(p.x(), p.y());
	return {
		g("Side by Side", {1, 1}, {1}, {s(0, 0), s(1, 0)}),
		g("Three Across", {1, 1, 1}, {1}, {s(0, 0), s(1, 0), s(2, 0)}),
		g("2x2 Grid", {1, 1}, {1, 1}, {s(0, 0), s(1, 0), s(0, 1), s(1, 1)}),
		g("3x3 Grid", {1, 1, 1}, {1, 1, 1}, grid3x3),
		g("Main + Stacked", {2, 1}, {1, 1}, {s(0, 0, 1, 2), s(1, 0), s(1, 1)}),
		g("Main + Side Column (3)", {3, 1}, {1, 1, 1}, {s(0, 0, 1, 3), s(1, 0), s(1, 1), s(1, 2)}),
		g("Main + Side Column (4)", {4, 1}, {1, 1, 1, 1}, {s(0, 0, 1, 4), s(1, 0), s(1, 1), s(1, 2), s(1, 3)}),
		g("Main + Bottom Strip", {1, 1, 1}, {3, 1}, {s(0, 0, 3, 1), s(0, 1), s(1, 1), s(2, 1)}),
		g("Main + Flanking", {1, 3, 1}, {1}, {s(1, 0), s(0, 0, 1, 1, 0), s(2, 0, 1, 1, 0)}),
		g("L-Shape (1 + 5)", {1, 1, 1}, {1, 1, 1}, lShape),
		g("Screen + Facecam", {3, 1}, {1}, {s(0, 0), s(1, 0, 1, 1, 0)}),
		g("Talk Show", {1, 1.4, 1}, {1}, {s(1, 0), s(0, 0, 1, 1, 0), s(2, 0, 1, 1, 0)}),
		g("Versus", {1, 1, 1, 1}, {2, 1}, {s(0, 0, 2, 1), s(2, 0, 2, 1), s(0, 1), s(3, 1)}),
		f("Interview", {s(0, 0, 0.65, 1), s(0.65, 0, 0.35, 1)}),
		f("Picture in Picture", {s(0, 0), s(0.72, 0.7, 0.25, 0.25)}, 0),
		f("Two Corner PiP", {s(0, 0), s(0.03, 0.7, 0.25, 0.25), s(0.72, 0.7, 0.25, 0.25)}, 0),
		g("Vertical: Game + Cam", {1}, {1, 1}, {s(0, 0), s(0, 1, 1, 1, 0)}),
	};
}

static QString configPath()
{
	char *p = obs_module_config_path("layouts.json");
	const QString s = QString::fromUtf8(p);
	bfree(p);
	return s;
}

QVector<Layout> readLayoutFile(const QString &path)
{
	QVector<Layout> v;
	QFile f(path);
	if (f.open(QIODevice::ReadOnly))
		for (const auto &x : QJsonDocument::fromJson(f.readAll()).array())
			v += Layout::fromJson(x.toObject());
	return v;
}

bool writeLayoutFile(const QString &path, const QVector<Layout> &list)
{
	QJsonArray a;
	for (const auto &L : list)
		a += L.toJson();
	QDir().mkpath(QFileInfo(path).path());
	QFile f(path);
	return f.open(QIODevice::WriteOnly) && f.write(QJsonDocument(a).toJson()) >= 0;
}

QVector<Layout> &layouts()
{
	static QVector<Layout> list = [] {
		auto v = readLayoutFile(configPath());
		return v.isEmpty() ? builtinLayouts() : v;
	}();
	return list;
}

void saveLayouts()
{
	writeLayoutFile(configPath(), layouts());
}
