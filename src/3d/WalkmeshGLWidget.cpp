/****************************************************************************
 ** Deling Final Fantasy VIII Field Editor
 ** Copyright (C) 2009-2024 Arzel Jérôme <myst6re@gmail.com>
 **
 ** This program is free software: you can redistribute it and/or modify
 ** it under the terms of the GNU General Public License as published by
 ** the Free Software Foundation, either version 3 of the License, or
 ** (at your option) any later version.
 **
 ** This program is distributed in the hope that it will be useful,
 ** but WITHOUT ANY WARRANTY; without even the implied warranty of
 ** MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 ** GNU General Public License for more details.
 **
 ** You should have received a copy of the GNU General Public License
 ** along with this program.  If not, see <http://www.gnu.org/licenses/>.
 ****************************************************************************/
#include "WalkmeshGLWidget.h"
#include <cmath>
#include <limits>

// Walkmesh coordinates are drawn divided by 4096
static QVector3D toScene(const Vertex_sr &point)
{
	return QVector3D(point.x / 4096.0f, point.y / 4096.0f, point.z / 4096.0f);
}

static qreal distanceToSegment(const QPointF &p, const QPointF &a, const QPointF &b)
{
	const QPointF ab = b - a;
	const qreal lengthSquared = QPointF::dotProduct(ab, ab);
	const qreal t = lengthSquared > 0.0 ? qBound(0.0, QPointF::dotProduct(p - a, ab) / lengthSquared, 1.0) : 0.0;

	return QLineF(p, a + ab * t).length();
}

WalkmeshGLWidget::WalkmeshGLWidget(QWidget *parent)
    : QOpenGLWidget(parent),
      distance(0.0), xRot(0.0f), yRot(0.0f), zRot(0.0f),
      xTrans(0.0f), yTrans(0.0f), transStep(360.0f), lastKeyPressed(-1),
      camID(0), _selectedTriangle(-1), _selectedDoor(-1), _selectedGate(-1),
      _lineToDrawPoint1(Vertex()), _lineToDrawPoint2(Vertex()),
      fovy(70.0), data(nullptr), curFrame(0), gpuRenderer(nullptr), _drawLine(false),
      _backgroundVisible(true), _editable(false), _dragging(false), _panning(false)
{
	// setMouseTracking(true);
	// startTimer(100);
}

WalkmeshGLWidget::~WalkmeshGLWidget()
{
	if (gpuRenderer != nullptr) {
		delete gpuRenderer;
	}
}

void WalkmeshGLWidget::timerEvent(QTimerEvent *)
{
	update();
}

void WalkmeshGLWidget::clear()
{
	data = nullptr;
	tex = QImage();
	clearPointSelection();

	update();
	
	if (gpuRenderer) {
		gpuRenderer->reset();
	}
}

void WalkmeshGLWidget::fill(Field *data)
{
	this->data = data;
	// A field opened from loose files may have a walkmesh and no background at all
	tex = data->hasBackgroundFile() ? data->getBackgroundFile()->background() : QImage();
	clearPointSelection();
	updatePerspective();
	resetCamera();
}

void WalkmeshGLWidget::setEditable(bool editable)
{
	_editable = editable;
	// Highlighting the point under the mouse needs move events with no button held
	setMouseTracking(editable);

	if (!editable) {
		clearPointSelection();
	}
}

void WalkmeshGLWidget::clearPointSelection()
{
	_hoveredPoint.reset();
	_selectedPoint.reset();
	_draggedPoint.reset();
	_sidePreview.reset();
	_dragging = false;
	update();
}

/**
 * How much of the widget the game screen occupies, keeping its aspect: the background and the
 * walkmesh both have to be drawn inside this rectangle or they cannot line up.
 */
void WalkmeshGLWidget::screenLetterbox(float &sx, float &sy) const
{
	const float widgetAspect = float(width()) / float(height()),
	            screenAspect = float(SCREEN_WIDTH) / float(SCREEN_HEIGHT);

	if (widgetAspect > screenAspect) {
		sx = screenAspect / widgetAspect;   // pillarbox
		sy = 1.0f;
	} else {
		sx = 1.0f;
		sy = widgetAspect / screenAspect;   // letterbox
	}
}
void WalkmeshGLWidget::computeFov()
{
	if (data && data->hasCaFile()
			&& data->getCaFile()->cameraCount() > 0
			&& camID < data->getCaFile()->cameraCount()) {
		const Camera &cam = data->getCaFile()->camera(camID);
		// The game projects onto the 320x224 screen with its centre at (160, 112), so the
		// vertical half-extent is 112 - not the 240 projection-plane height of the PSX GTE.
		fovy = (2 * atan(112.0/cam.camera_zoom)) * 57.29577951;
	} else {
		fovy = 70.0;
	}
}

void WalkmeshGLWidget::updatePerspective()
{
	computeFov();
	resizeGL(width(), height());
	update();
}

void WalkmeshGLWidget::initializeGL()
{
}

void WalkmeshGLWidget::resizeGL(int width, int height)
{
	if (gpuRenderer != nullptr) {
		gpuRenderer->setViewport(0, 0, width, height);
	}
}

void WalkmeshGLWidget::paintGL()
{
	if (!data) {
		return;
	}

	if (gpuRenderer == nullptr) {
		gpuRenderer = new Renderer(this);
	}

	if (gpuRenderer->hasError()) {
		return;
	}

	gpuRenderer->clear();

	if (_backgroundVisible) {
		drawBackground();
	}

	gpuRenderer->bindProjectionMatrix(projectionMatrix());
	gpuRenderer->bindModelMatrix(modelMatrix());
	gpuRenderer->bindViewMatrix(viewMatrix());

	if (data->hasIdFile()) {
		IdFile *idFile = data->getIdFile();
		int i=0;

		for (const Triangle &triangle: idFile->getTriangles()) {
			const Access &access = idFile->access(i);
			// Red: a triangle the game cannot use - flipped (never walked on), flat (drops the
			// player to height 0) or past the last id the script can lock
			const bool broken = IdFile::isFlipped(triangle) || IdFile::isFlat(triangle)
			                    || i >= IdFile::MAX_TRIANGLES;

			for (int side = 0; side < 3; ++side) {
				QRgb color = i == _selectedTriangle ? 0xFFFF9000
				             : broken ? 0xFFFF2020
				             : access.a[side] == -1 ? 0xFF6699CC : 0xFFFFFFFF;
				bufferLine(triangle.vertices[side], triangle.vertices[(side + 1) % 3], QRgba64::fromArgb32(color));
			}

			++i;
		}

		// The triangle a click would add, dashed until it exists
		if (_sidePreview) {
			const Triangle &source = idFile->triangle(_sidePreview->triangleID);
			const QRgba64 color = QRgba64::fromArgb32(0xFFFFE040);
			bufferLine(source.vertices[_sidePreview->side], _sidePreview->point, color, true);
			bufferLine(source.vertices[(_sidePreview->side + 1) % 3], _sidePreview->point, color, true);
		}

		if (!_drawLine && data->hasInfFile()) {
			InfFile *inf = data->getInfFile();

			for (const Gateway &gate: inf->getGateways()) {
				if (gate.fieldId != 0x7FFF) {
					// Vertex info
					QVector3D positionA(gate.exitLine[0].x / 4096.0, gate.exitLine[0].y / 4096.0, gate.exitLine[0].z / 4096.0),
										positionB(gate.exitLine[1].x / 4096.0, gate.exitLine[1].y / 4096.0, gate.exitLine[1].z / 4096.0);
					QRgba64   color = QRgba64::fromArgb32(0xFFFF0000);
					QVector2D texcoord;

					gpuRenderer->bufferVertex(positionA, color, texcoord);
					gpuRenderer->bufferVertex(positionB, color, texcoord);
				}
			}

			for (const Trigger &trigger: inf->getTriggers()) {
				if (trigger.doorID != 0xFF) {
					// Vertex info
					QVector3D positionA(trigger.trigger_line[0].x / 4096.0, trigger.trigger_line[0].y / 4096.0, trigger.trigger_line[0].z / 4096.0),
										positionB(trigger.trigger_line[1].x / 4096.0, trigger.trigger_line[1].y / 4096.0, trigger.trigger_line[1].z / 4096.0);
					QRgba64   color = QRgba64::fromArgb32(0xFF00FF00);
					QVector2D texcoord;

					gpuRenderer->bufferVertex(positionA, color, texcoord);
					gpuRenderer->bufferVertex(positionB, color, texcoord);
				}
			}
		}

		if (_drawLine) {
			// Vertex info
			QVector3D positionA(_lineToDrawPoint1.x / 4096.0, _lineToDrawPoint1.y / 4096.0, _lineToDrawPoint1.z / 4096.0),
								positionB(_lineToDrawPoint2.x / 4096.0, _lineToDrawPoint2.y / 4096.0, _lineToDrawPoint2.z / 4096.0);
			QRgba64   color = QRgba64::fromArgb32(0xFFFF00FF);
			QVector2D texcoord;

			gpuRenderer->bufferVertex(positionA, color, texcoord);
			gpuRenderer->bufferVertex(positionB, color, texcoord);
		}

		gpuRenderer->draw(RendererPrimitiveType::PT_LINES);

		if (_selectedTriangle >= 0 && _selectedTriangle < data->getIdFile()->triangleCount()) {
			const Triangle &triangle = data->getIdFile()->triangle(_selectedTriangle);

			// Vertex info
			QVector3D positionA(triangle.vertices[0].x / 4096.0, triangle.vertices[0].y / 4096.0, triangle.vertices[0].z / 4096.0),
								positionB(triangle.vertices[1].x / 4096.0, triangle.vertices[1].y / 4096.0, triangle.vertices[1].z / 4096.0),
								positionC(triangle.vertices[2].x / 4096.0, triangle.vertices[2].y / 4096.0, triangle.vertices[2].z / 4096.0);
			QRgba64   color = QRgba64::fromArgb32(0xFFFF9000);
			QVector2D texcoord;

			// Line
			gpuRenderer->bufferVertex(positionA, color, texcoord);
			gpuRenderer->bufferVertex(positionB, color, texcoord);
			gpuRenderer->bufferVertex(positionC, color, texcoord);
		}

		if (data->hasInfFile()) {
			if (_selectedGate >= 0 && _selectedGate < 12) {
				const Gateway &gate = data->getInfFile()->getGateway(_selectedGate);
				if (gate.fieldId != 0x7FFF) {
					// Vertex info
					QVector3D positionA(gate.exitLine[0].x / 4096.0, gate.exitLine[0].y / 4096.0, gate.exitLine[0].z / 4096.0),
										positionB(gate.exitLine[1].x / 4096.0, gate.exitLine[1].y / 4096.0, gate.exitLine[1].z / 4096.0);
					QRgba64   color = QRgba64::fromArgb32(0xFFFF0000);
					QVector2D texcoord;

					gpuRenderer->bufferVertex(positionA, color, texcoord);
					gpuRenderer->bufferVertex(positionB, color, texcoord);
				}
			}

			if (_selectedDoor >= 0 && _selectedDoor < 12) {
				const Trigger &trigger = data->getInfFile()->getTrigger(_selectedDoor);
				if (trigger.doorID != 0xFF) {
					// Vertex info
					QVector3D positionA(trigger.trigger_line[0].x / 4096.0, trigger.trigger_line[0].y / 4096.0, trigger.trigger_line[0].z / 4096.0),
										positionB(trigger.trigger_line[1].x / 4096.0, trigger.trigger_line[1].y / 4096.0, trigger.trigger_line[1].z / 4096.0);
					QRgba64   color = QRgba64::fromArgb32(0xFF00FF00);
					QVector2D texcoord;

					gpuRenderer->bufferVertex(positionA, color, texcoord);
					gpuRenderer->bufferVertex(positionB, color, texcoord);
				}
			}
		}

		gpuRenderer->draw(RendererPrimitiveType::PT_POINTS, 7.0f);

		// The selected point (orange), and what a click would grab or add (yellow)
		const QVector2D texcoord;
		if (_selectedPoint) {
			gpuRenderer->bufferVertex(toScene(*_selectedPoint), QRgba64::fromArgb32(0xFFFF9000), texcoord);
		}
		if (_hoveredPoint) {
			gpuRenderer->bufferVertex(toScene(*_hoveredPoint), QRgba64::fromArgb32(0xFFFFE040), texcoord);
		}
		if (_sidePreview) {
			gpuRenderer->bufferVertex(toScene(_sidePreview->point), QRgba64::fromArgb32(0xFFFFE040), texcoord);
		}
		gpuRenderer->draw(RendererPrimitiveType::PT_POINTS, 11.0f);
	}
}

QMatrix4x4 WalkmeshGLWidget::projectionMatrix() const
{
	// The mesh must land in the same rectangle as the background, so project with the
	// SCREEN aspect and letterbox that rectangle into the widget - using the widget's own
	// aspect made the mesh drift sideways from the background on any non-4:3 window.
	float sx = 1.0f, sy = 1.0f;
	screenLetterbox(sx, sy);

	QMatrix4x4 projection;
	projection.scale(sx, sy, 1.0f);
	projection.perspective(fovy, float(SCREEN_WIDTH) / float(SCREEN_HEIGHT), 0.001f, 1000.0f);

	return projection;
}

QMatrix4x4 WalkmeshGLWidget::viewMatrix() const
{
	QMatrix4x4 view;

	if (data && data->hasCaFile() && data->getCaFile()->cameraCount() > 0 && camID < data->getCaFile()->cameraCount()) {
		const Camera &cam = data->getCaFile()->camera(camID);

		double camAxisXx = cam.camera_axis[0].x / 4096.0;
		double camAxisXy = cam.camera_axis[0].y / 4096.0;
		double camAxisXz = cam.camera_axis[0].z / 4096.0;

		double camAxisYx = -cam.camera_axis[1].x / 4096.0;
		double camAxisYy = -cam.camera_axis[1].y / 4096.0;
		double camAxisYz = -cam.camera_axis[1].z / 4096.0;

		double camAxisZx = cam.camera_axis[2].x / 4096.0;
		double camAxisZy = cam.camera_axis[2].y / 4096.0;
		double camAxisZz = cam.camera_axis[2].z / 4096.0;

		double camPosX = cam.camera_position[0] / 4096.0;
		double camPosY = -cam.camera_position[1] / 4096.0;
		double camPosZ = cam.camera_position[2] / 4096.0;

		double tx = -(camPosX*camAxisXx + camPosY*camAxisYx + camPosZ*camAxisZx);
		double ty = -(camPosX*camAxisXy + camPosY*camAxisYy + camPosZ*camAxisZy);
		double tz = -(camPosX*camAxisXz + camPosY*camAxisYz + camPosZ*camAxisZz);

		const QVector3D eye(tx, ty, tz), center(tx + camAxisZx, ty + camAxisZy, tz + camAxisZz), up(camAxisYx, camAxisYy, camAxisYz);
		view.lookAt(eye, center, up);
	}

	return view;
}

QMatrix4x4 WalkmeshGLWidget::modelMatrix() const
{
	QMatrix4x4 model;
	model.translate(xTrans, yTrans, distance);
	model.rotate(xRot, 1.0f, 0.0f, 0.0f);
	model.rotate(yRot, 0.0f, 1.0f, 0.0f);
	model.rotate(zRot, 0.0f, 0.0f, 1.0f);

	return model;
}

// Everything picking needs goes through the matrices used for drawing, so what a click hits is
// always exactly what is on screen
QMatrix4x4 WalkmeshGLWidget::sceneToClip() const
{
	return projectionMatrix() * viewMatrix() * modelMatrix();
}

void WalkmeshGLWidget::bufferLine(const Vertex_sr &from, const Vertex_sr &to, QRgba64 color, bool dashed)
{
	const QVector3D a = toScene(from), b = toScene(to);
	const QVector2D texcoord;

	if (!dashed) {
		gpuRenderer->bufferVertex(a, color, texcoord);
		gpuRenderer->bufferVertex(b, color, texcoord);
		return;
	}

	// Cut the line in DASHES pieces and draw every other one
	const int DASHES = 11;
	for (int i = 0; i < DASHES; i += 2) {
		gpuRenderer->bufferVertex(a + (b - a) * (float(i) / DASHES), color, texcoord);
		gpuRenderer->bufferVertex(a + (b - a) * (float(i + 1) / DASHES), color, texcoord);
	}
}

bool WalkmeshGLWidget::toScreen(const QMatrix4x4 &sceneToClip, const Vertex_sr &point, QPointF &screen) const
{
	const QVector4D clip = sceneToClip * QVector4D(toScene(point), 1.0f);

	if (clip.w() <= 0.0f) {
		return false; // behind the camera
	}

	screen = QPointF((clip.x() / clip.w() + 1.0) * 0.5 * width(),
	                 (1.0 - clip.y() / clip.w()) * 0.5 * height());

	return true;
}

/**
 * Where the mouse points on the horizontal plane at `planeHeight`: a point is dragged along the
 * floor at its own height, so it moves over the ground seen through the game camera.
 */
bool WalkmeshGLWidget::mouseOnHeight(const QPoint &pos, qint16 planeHeight, Vertex_sr &point) const
{
	bool invertible = false;
	const QMatrix4x4 clipToScene = sceneToClip().inverted(&invertible);

	if (!invertible) {
		return false;
	}

	// The mouse ray, from the near plane to the far plane
	const float ndcX = 2.0f * pos.x() / width() - 1.0f, ndcY = 1.0f - 2.0f * pos.y() / height();
	const QVector4D nearClip = clipToScene * QVector4D(ndcX, ndcY, -1.0f, 1.0f),
	                farClip = clipToScene * QVector4D(ndcX, ndcY, 1.0f, 1.0f);
	const QVector3D nearPoint = nearClip.toVector3D() / nearClip.w(),
	                farPoint = farClip.toVector3D() / farClip.w();

	const float planeZ = planeHeight / 4096.0f, dz = farPoint.z() - nearPoint.z();

	if (qFuzzyIsNull(dz)) {
		return false; // looking along the plane
	}

	const QVector3D hit = nearPoint + (farPoint - nearPoint) * ((planeZ - nearPoint.z()) / dz);

	point.x = qint16(qBound(-32768.0f, std::round(hit.x() * 4096.0f), 32767.0f));
	point.y = qint16(qBound(-32768.0f, std::round(hit.y() * 4096.0f), 32767.0f));
	point.z = planeHeight;
	point.res = 0;

	return true;
}

std::optional<Vertex_sr> WalkmeshGLWidget::pointAt(const QPoint &pos, const std::optional<Vertex_sr> &ignored) const
{
	if (!data || !data->hasIdFile()) {
		return std::nullopt;
	}

	const QMatrix4x4 mvp = sceneToClip();
	std::optional<Vertex_sr> nearest;
	qreal nearestDistance = PICK_RADIUS;

	for (const Triangle &triangle: data->getIdFile()->getTriangles()) {
		for (const Vertex_sr &vertex: triangle.vertices) {
			QPointF screen;

			if ((ignored && IdFile::samePoint(vertex, *ignored)) || !toScreen(mvp, vertex, screen)) {
				continue;
			}

			const qreal distance = QLineF(screen, QPointF(pos)).length();
			if (distance <= nearestDistance) {
				nearestDistance = distance;
				nearest = vertex;
			}
		}
	}

	return nearest;
}

bool WalkmeshGLWidget::isOnFloor(const QPoint &pos) const
{
	const QMatrix4x4 mvp = sceneToClip();

	for (const Triangle &triangle: data->getIdFile()->getTriangles()) {
		QPolygonF polygon;
		QPointF screen;
		bool visible = true;

		for (const Vertex_sr &vertex: triangle.vertices) {
			if (!toScreen(mvp, vertex, screen)) {
				visible = false;
				break;
			}
			polygon << screen;
		}

		if (visible && polygon.containsPoint(QPointF(pos), Qt::OddEvenFill)) {
			return true;
		}
	}

	return false;
}

/**
 * The triangle a click would add: on the border side (a wall) nearest to the mouse. Attaching
 * to a side, rather than to the two nearest points, always makes a clean triangle - the two
 * nearest points are not necessarily joined, and a triangle between them could cross others.
 */
std::optional<WalkmeshGLWidget::SidePreview> WalkmeshGLWidget::sidePreviewAt(const QPoint &pos) const
{
	const QMatrix4x4 mvp = sceneToClip();
	IdFile *idFile = data->getIdFile();
	int nearestTriangle = -1, nearestSide = -1;
	qreal nearestDistance = std::numeric_limits<qreal>::max();

	for (int triangleID = 0; triangleID < idFile->triangleCount(); ++triangleID) {
		const Triangle &triangle = idFile->triangle(triangleID);

		for (int side = 0; side < 3; ++side) {
			QPointF a, b;

			if (idFile->access(triangleID).a[side] != -1
			        || !toScreen(mvp, triangle.vertices[side], a)
			        || !toScreen(mvp, triangle.vertices[(side + 1) % 3], b)) {
				continue;
			}

			const qreal distance = distanceToSegment(QPointF(pos), a, b);
			if (distance < nearestDistance) {
				nearestDistance = distance;
				nearestTriangle = triangleID;
				nearestSide = side;
			}
		}
	}

	if (nearestTriangle < 0) {
		return std::nullopt;
	}

	// The new point sits at the height of the side it is attached to
	const Triangle &triangle = idFile->triangle(nearestTriangle);
	const qint16 height = qint16((triangle.vertices[nearestSide].z + triangle.vertices[(nearestSide + 1) % 3].z) / 2);
	Vertex_sr point;

	if (!mouseOnHeight(pos, height, point)) {
		return std::nullopt;
	}

	return SidePreview{nearestTriangle, nearestSide, point};
}

void WalkmeshGLWidget::updateHover(const QPoint &pos)
{
	_hoveredPoint.reset();
	_sidePreview.reset();

	if (_editable && data && data->hasIdFile()) {
		_hoveredPoint = pointAt(pos);

		// Grabbing a point wins, and adding is only offered off the floor, so clicking on the
		// floor never creates a triangle by accident
		if (!_hoveredPoint && !isOnFloor(pos)) {
			_sidePreview = sidePreviewAt(pos);
		}
	}

	update();
}

void WalkmeshGLWidget::drawBackground()
{
	if (data->getBackgroundFile())
	{
		// Map the image so its central SCREEN_WIDTH x SCREEN_HEIGHT region covers exactly the
		// same rectangle the walkmesh is projected into; a bigger (scrolling) background then
		// simply extends past it instead of being squeezed to fit.
		float sx = 1.0f, sy = 1.0f;
		screenLetterbox(sx, sy);
		const float bx = tex.isNull() ? sx : sx * float(tex.width()) / float(SCREEN_WIDTH),
		            by = tex.isNull() ? sy : sy * float(tex.height()) / float(SCREEN_HEIGHT);

		RendererVertex vertices[] = {
		    {
		        {-bx, -by, 1.0f, 1.0f},
		        {1.0f, 1.0f, 1.0f, 1.0f},
		        {0.0f, 1.0f},
		    },
		    {
		        {-bx, by, 1.0f, 1.0f},
		        {1.0f, 1.0f, 1.0f, 1.0f},
		        {0.0f, 0.0f},
		    },
		    {
		        {bx, -by, 1.0f, 1.0f},
		        {1.0f, 1.0f, 1.0f, 1.0f},
		        {1.0f, 1.0f},
		    },
		    {
		        {bx, by, 1.0f, 1.0f},
		        {1.0f, 1.0f, 1.0f, 1.0f},
		        {1.0f, 0.0f},
		    }
		};

		uint32_t indices[] = {
		    0, 1, 2,
		    1, 3, 2
		};

		QMatrix4x4 mBG;

		gpuRenderer->bindProjectionMatrix(mBG);
		gpuRenderer->bindViewMatrix(mBG);
		gpuRenderer->bindModelMatrix(mBG);

		gpuRenderer->bindVertex(vertices, 4);
		gpuRenderer->bindIndex(indices, 6);
		gpuRenderer->bindTexture(tex);
		gpuRenderer->draw(RendererPrimitiveType::PT_TRIANGLES);
	}
}

void WalkmeshGLWidget::wheelEvent(QWheelEvent *event)
{
	setFocus();
	// angleDelta() is what a regular mouse wheel reports: pixelDelta() stays null for one on
	// Windows, and its horizontal component was read, so the wheel did nothing there
	distance += event->angleDelta().y() / 4096.0;
	update();
}

void WalkmeshGLWidget::mousePressEvent(QMouseEvent *event)
{
	setFocus();
	if (event->button() == Qt::MiddleButton)
	{
		distance = -35;
		update();
	}
	else if (event->button() == Qt::RightButton)
	{
		// The view moves with the right button, leaving the left one to edit the walkmesh
		moveStart = event->pos();
		_panning = true;
	}
	else if (event->button() == Qt::LeftButton && _editable)
	{
		updateHover(event->pos());

		if (_hoveredPoint) {
			_selectedPoint = _draggedPoint = _hoveredPoint;
			_dragging = true;
			emit pointSelected(*_selectedPoint);
			emit pointDragStarted();
		} else if (_sidePreview) {
			const SidePreview preview = *_sidePreview;
			_sidePreview.reset();
			_selectedPoint = preview.point;
			emit pointAddRequested(preview.triangleID, preview.side, preview.point);
			emit pointSelected(preview.point);
		} else {
			_selectedPoint.reset();
		}

		update();
	}
}

void WalkmeshGLWidget::mouseMoveEvent(QMouseEvent *event)
{
	// buttons(), not button(): for a move event button() is always Qt::NoButton, which is why
	// dragging never moved the view before
	if (_panning && (event->buttons() & Qt::RightButton)) {
		xTrans += (event->pos().x() - moveStart.x()) / 4096.0;
		yTrans -= (event->pos().y() - moveStart.y()) / 4096.0;
		moveStart = event->pos();
		update();
	} else if (_dragging && (event->buttons() & Qt::LeftButton)) {
		Vertex_sr target;

		if (mouseOnHeight(event->pos(), _draggedPoint->z, target) && !IdFile::samePoint(target, *_draggedPoint)) {
			emit pointDragged(*_draggedPoint, target);
			_selectedPoint = _draggedPoint = target;
		}

		// Releasing near another point merges the two: show which one, so it never happens
		// by surprise in a dense part of the walkmesh
		_hoveredPoint = pointAt(event->pos(), _draggedPoint);
		update();
	} else {
		updateHover(event->pos());
	}
}

void WalkmeshGLWidget::mouseReleaseEvent(QMouseEvent *event)
{
	if (event->button() == Qt::RightButton) {
		_panning = false;
	} else if (event->button() == Qt::LeftButton && _dragging) {
		// Dropped onto another point: snap to it, which merges the two
		const std::optional<Vertex_sr> target = pointAt(event->pos(), _draggedPoint);
		if (target) {
			emit pointDragged(*_draggedPoint, *target);
			_selectedPoint = target;
		}

		_dragging = false;
		_draggedPoint.reset();
		emit pointDragFinished();
		updateHover(event->pos());
	}
}

void WalkmeshGLWidget::leaveEvent(QEvent *event)
{
	_hoveredPoint.reset();
	_sidePreview.reset();
	update();
	QOpenGLWidget::leaveEvent(event);
}

void WalkmeshGLWidget::keyPressEvent(QKeyEvent *event)
{
	// This widget grabs the keyboard while it has the focus, so shortcuts defined elsewhere would
	// never fire: the editing keys are handled here
	if (event->matches(QKeySequence::Undo)) {
		emit undoRequested();
		return;
	}
	if (event->matches(QKeySequence::Redo)) {
		emit redoRequested();
		return;
	}
	if (event->key() == Qt::Key_Delete) {
		if (_editable && _selectedPoint) {
			const Vertex_sr point = *_selectedPoint;
			clearPointSelection();
			emit pointDeleteRequested(point);
		}
		return;
	}
	if (event->key() == Qt::Key_Escape) {
		clearPointSelection();
		return;
	}

	if (lastKeyPressed == event->key()
			&& (event->key() == Qt::Key_Left
				|| event->key() == Qt::Key_Right
				|| event->key() == Qt::Key_Down
				|| event->key() == Qt::Key_Up)) {
		if (transStep > 100.0f) {
			transStep *= 0.90f; // accelerator
		}
	} else {
		transStep = 360.0f;
	}
	lastKeyPressed = event->key();

	switch (event->key())
	{
	case Qt::Key_Left:
		xTrans += 1.0f/transStep;
		update();
		break;
	case Qt::Key_Right:
		xTrans -= 1.0f/transStep;
		update();
		break;
	case Qt::Key_Down:
		yTrans += 1.0f/transStep;
		update();
		break;
	case Qt::Key_Up:
		yTrans -= 1.0f/transStep;
		update();
		break;
	default:
		QWidget::keyPressEvent(event);
		return;
	}
}

void WalkmeshGLWidget::focusInEvent(QFocusEvent *event)
{
	grabKeyboard();
	QWidget::focusInEvent(event);
}

void WalkmeshGLWidget::focusOutEvent(QFocusEvent *event)
{
	releaseKeyboard();
	QWidget::focusOutEvent(event);
}

static void qNormalizeAngle(int &angle)
{
	while (angle < 0)
		angle += 360 * 16;
	while (angle > 360 * 16)
		angle -= 360 * 16;
}

void WalkmeshGLWidget::setXRotation(int angle)
{
	qNormalizeAngle(angle);
	if (angle != xRot) {
		xRot = angle;
		update();
	}
}

void WalkmeshGLWidget::setYRotation(int angle)
{
	qNormalizeAngle(angle);
	if (angle != yRot) {
		yRot = angle;
		update();
	}
}

void WalkmeshGLWidget::setZRotation(int angle)
{
	qNormalizeAngle(angle);
	if (angle != zRot) {
		zRot = angle;
		update();
	}
}

void WalkmeshGLWidget::setZoom(int zoom)
{
	distance = zoom / 4096.0;
}

void WalkmeshGLWidget::resetCamera()
{
	distance = 0;
	zRot = yRot = xRot = 0;
	xTrans = yTrans = 0;
	update();
}

void WalkmeshGLWidget::setCurrentFieldCamera(int camID)
{
	this->camID = camID;
	updatePerspective();
}

void WalkmeshGLWidget::setSelectedTriangle(int triangle)
{
	_selectedTriangle = triangle;
	update();
}

void WalkmeshGLWidget::setSelectedDoor(int door)
{
	_selectedDoor = door;
	update();
}

void WalkmeshGLWidget::setSelectedGate(int gate)
{
	_selectedGate = gate;
	update();
}

void WalkmeshGLWidget::setLineToDraw(const Vertex vertex[2])
{
	_lineToDrawPoint1 = vertex[0];
	_lineToDrawPoint2 = vertex[1];
	_drawLine = true;
	update();
}

void WalkmeshGLWidget::clearLineToDraw()
{
	_drawLine = false;
	update();
}

void WalkmeshGLWidget::setBackgroundVisible(bool show)
{
	if (_backgroundVisible != show) {
		_backgroundVisible = show;
		update();
	}
}
