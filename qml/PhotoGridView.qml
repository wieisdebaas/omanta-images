import QtQuick
import QtQuick.Controls
import Omanta.Runtime

// Large-tile photo browser (Ctrl+3). Virtualized GridView + async photo
// thumbnails (disk/RAM cache, priority by viewport).
Item {
    id: root

    required property var tab
    property alias currentIndex: view.currentIndex
    readonly property int tileSize: Math.max(160, root.tab.zoom * 2)
    readonly property int cellWidth: tileSize + 24
    readonly property int cellHeight: tileSize + 48
    readonly property int columns: Math.max(1, Math.floor(view.width / cellWidth))

    property string activePath: ""
    property string activeName: ""

    function positionAt(row) { view.positionViewAtIndex(row, GridView.Contain); }

    GridView {
        id: view

        anchors.fill: parent
        anchors.margins: 8
        model: root.tab.files
        cellWidth: root.cellWidth
        cellHeight: root.cellHeight
        clip: true
        focus: true
        keyNavigationEnabled: false
        boundsBehavior: Flickable.StopAtBounds

        // Prefetch ~1–3 viewport heights so scrolling stays smooth while
        // discovery continues on SMB.
        readonly property real prefetchDistance:
            height * (Math.abs(verticalVelocity) > 2200 ? 3 : 1.5)
        displayMarginBeginning: verticalVelocity < -100 ? prefetchDistance : height * 1.0
        displayMarginEnd: verticalVelocity > 100 ? prefetchDistance : height * 1.0

        ScrollBar.vertical: ScrollBar {}

        delegate: Item {
            id: cell

            required property int index
            required property string name
            required property string displayName
            required property string filePath
            required property string targetPath
            required property string contentType
            required property real size
            required property var modified

            width: view.cellWidth
            height: view.cellHeight

            readonly property string previewPath:
                cell.targetPath !== "" ? cell.targetPath : cell.filePath
            readonly property bool inViewport:
                y + height >= view.contentY && y <= view.contentY + view.height
            readonly property bool nearViewport:
                y + height >= view.contentY - view.height
                && y <= view.contentY + view.height * 2
            // Thread-pool priority: visible high, prefetch medium, else low.
            readonly property int requestPriority:
                inViewport ? 100 : nearViewport ? 50 : 10
            readonly property string thumbnailSource:
                Thumbnails.photoSource(previewPath, cell.modified, cell.size,
                                       root.tileSize, requestPriority)

            Rectangle {
                anchors.fill: parent
                anchors.margins: 4
                radius: Colors.radius
                color: root.tab.isSelected(cell.name) ? Colors.selection
                     : mouse.containsMouse ? Colors.hover : Colors.chrome
            }

            Column {
                anchors.horizontalCenter: parent.horizontalCenter
                anchors.top: parent.top
                anchors.topMargin: 10
                spacing: 8
                width: root.tileSize

                Rectangle {
                    width: root.tileSize
                    height: root.tileSize
                    color: Colors.window
                    radius: 2

                    Image {
                        anchors.fill: parent
                        anchors.margins: 2
                        source: cell.thumbnailSource
                        sourceSize: Qt.size(root.tileSize, root.tileSize)
                        fillMode: Image.PreserveAspectFit
                        asynchronous: true
                        cache: false
                    }
                }

                Text {
                    textFormat: Text.PlainText
                    width: parent.width
                    text: cell.displayName
                    color: root.tab.isSelected(cell.name) ? Colors.selectionText : Colors.text
                    font.pixelSize: 12
                    horizontalAlignment: Text.AlignHCenter
                    elide: Text.ElideMiddle
                }
            }

            MouseArea {
                id: mouse
                anchors.fill: parent
                hoverEnabled: true
                acceptedButtons: Qt.LeftButton | Qt.RightButton
                onClicked: mouse => {
                    root.tab.setCurrent(cell.index, mouse.modifiers & Qt.ShiftModifier)
                    if (mouse.button === Qt.RightButton) {
                        root.tab.requestContextMenu()
                        return
                    }
                    root.activePath = cell.previewPath
                    root.activeName = cell.displayName
                    preview.open()
                }
                onDoubleClicked: root.tab.activate(cell.index)
            }
        }
    }

    Popup {
        id: preview

        anchors.centerIn: Overlay.overlay
        width: Overlay.overlay ? Math.min(Overlay.overlay.width - 48, 1200) : 800
        height: Overlay.overlay ? Math.min(Overlay.overlay.height - 48, 900) : 600
        modal: true
        focus: true
        padding: 0
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
        onClosed: {
            root.activePath = ""
            root.activeName = ""
        }

        background: Rectangle {
            color: Colors.window
            border.color: Colors.border
            radius: Colors.radius
        }

        contentItem: Item {
            Image {
                anchors.fill: parent
                anchors.margins: 24
                anchors.bottomMargin: 48
                source: preview.opened ? Thumbnails.originalSource(root.activePath) : ""
                fillMode: Image.PreserveAspectFit
                asynchronous: true
                cache: false
            }

            Text {
                textFormat: Text.PlainText
                anchors.left: parent.left
                anchors.bottom: parent.bottom
                anchors.margins: 16
                text: root.activeName
                color: Colors.text
                font.pixelSize: 13
            }

            DialogCloseButton {
                dialog: preview
            }
        }
    }
}
