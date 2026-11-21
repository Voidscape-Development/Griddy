#pragma once
#include <QJsonObject>
#include <QRectF>
#include <QString>
#include <QVector>

enum Fit { FitCrop, FitInside, FitStretch };
enum Orient { AnyOrient, Landscape, Portrait };

// Platform UI safe zones: name and insets (left, top, right, bottom) as canvas fractions. Index 0 = off.
struct SafeZone {
	const char *name;
	double l, t, r, b;
};
extern const QVector<SafeZone> kSafeZones;

struct Slot {
	QRectF r;          // grid: cell x, y, column span, row span | free: normalized 0..1 rect
	int fit = FitCrop; // how the source fills the slot
	double aspect = 0; // locked width/height ratio, 0 = unlocked
};

struct Layout {
	QString name, group = "Custom"; // group: gallery section, e.g. "Podcast"
	bool grid = true;
	QVector<double> cols{1}, rows{1}; // grid column/row weights
	QVector<Slot> slots;              // slot 0 is the main slot
	int gap = 10, margin = 0;
	bool aspectLock = true, mirror = false;
	int orient = AnyOrient, safeZone = 0;
	bool safeFit = false; // keep slots inside the safe zone

	QRectF safeRect(double W, double H) const;
	bool fits(double W, double H) const { return orient == AnyOrient || (orient == Portrait) == (H > W); }

	// Pixel rects for each slot on a W x H canvas.
	QVector<QRectF> compute(double W, double H) const;
	// Main slot's share (%) of the grid axis it partially spans, -1 if not applicable.
	int mainPct() const;
	void setMainPct(int pct);
	void toFree();
	void toGrid();
	QJsonObject toJson() const;
	static Layout fromJson(const QJsonObject &o);
};

QVector<Layout> builtinLayouts();
QVector<Layout> &layouts(); // global list, loaded on first use
void saveLayouts();
QVector<Layout> readLayoutFile(const QString &path);
bool writeLayoutFile(const QString &path, const QVector<Layout> &list);
