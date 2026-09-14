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
#pragma once

#include <QtWidgets>
#include <optional>
#include "Field.h"
#include "Renderer.h"

class WalkmeshGLWidget : public QOpenGLWidget
{
	Q_OBJECT
public:
	explicit WalkmeshGLWidget(QWidget *parent = nullptr);
	virtual ~WalkmeshGLWidget() override;
	void clear();
	void fill(Field *data);
	void updatePerspective();
	void setEditable(bool editable);
	void clearPointSelection();
signals:
	// Walkmesh edition with the mouse. This widget only works out what the user points at;
	// changing the walkmesh is left to the page, so every change goes through its undo stack.
	void pointSelected(const Vertex_sr &point);
	void pointDragStarted();
	void pointDragged(const Vertex_sr &from, const Vertex_sr &to);
	void pointDragFinished();
	void pointAddRequested(int triangleID, int side, const Vertex_sr &point);
	void pointDeleteRequested(const Vertex_sr &point);
	void undoRequested();
	void redoRequested();
public slots:
	void setXRotation(int);
	void setYRotation(int);
	void setZRotation(int);
	void setZoom(int);
	void resetCamera();
	void setCurrentFieldCamera(int camID);
	void setBackgroundVisible(bool show);
	void setSelectedTriangle(int triangle);
	void setSelectedDoor(int door);
	void setSelectedGate(int gate);
	void setLineToDraw(const Vertex vertex[2]);
	void clearLineToDraw();
private:
	// The screen the game projects a field into; its centre (160, 112) is the projection centre
	static const int SCREEN_WIDTH = 320, SCREEN_HEIGHT = 224;
	// How close to a point, in pixels, the mouse has to be to grab it
	static const int PICK_RADIUS = 10;
	// The triangle a click would add while the mouse is off the floor: a border side and a point
	struct SidePreview {
		int triangleID;
		int side;
		Vertex_sr point;
	};
	void screenLetterbox(float &sx, float &sy) const;
	void computeFov();
	void drawBackground();
	QMatrix4x4 projectionMatrix() const;
	QMatrix4x4 viewMatrix() const;
	QMatrix4x4 modelMatrix() const;
	QMatrix4x4 sceneToClip() const;
	bool toScreen(const QMatrix4x4 &sceneToClip, const Vertex_sr &point, QPointF &screen) const;
	bool mouseOnHeight(const QPoint &pos, qint16 planeHeight, Vertex_sr &point) const;
	std::optional<Vertex_sr> pointAt(const QPoint &pos, const std::optional<Vertex_sr> &ignored = std::nullopt) const;
	bool isOnFloor(const QPoint &pos) const;
	std::optional<SidePreview> sidePreviewAt(const QPoint &pos) const;
	void updateHover(const QPoint &pos);
	void bufferLine(const Vertex_sr &from, const Vertex_sr &to, QRgba64 color, bool dashed = false);
	double distance;
	float xRot, yRot, zRot;
	float xTrans, yTrans, transStep;
	int lastKeyPressed;
	int camID;
	int _selectedTriangle;
	int _selectedDoor;
	int _selectedGate;
	Vertex _lineToDrawPoint1, _lineToDrawPoint2;
	double fovy;
	Field *data;
	QPoint moveStart;
	int curFrame;
	Renderer *gpuRenderer;
	QImage tex;
	bool _drawLine;
	bool _backgroundVisible;
	bool _editable, _dragging, _panning;
	std::optional<Vertex_sr> _hoveredPoint, _selectedPoint, _draggedPoint;
	std::optional<SidePreview> _sidePreview;

protected:
	virtual void timerEvent(QTimerEvent *event) override;
	virtual void initializeGL() override;
	virtual void resizeGL(int w, int h) override;
	virtual void paintGL() override;
	virtual void wheelEvent(QWheelEvent *event) override;
	virtual void mousePressEvent(QMouseEvent *event) override;
	virtual void mouseMoveEvent(QMouseEvent *event) override;
	virtual void mouseReleaseEvent(QMouseEvent *event) override;
	virtual void leaveEvent(QEvent *event) override;
	virtual void keyPressEvent(QKeyEvent *event) override;
	virtual void focusInEvent(QFocusEvent *event) override;
	virtual void focusOutEvent(QFocusEvent *event) override;
};
