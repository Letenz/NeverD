pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic
import NeverD.Native 1.0

Rectangle {
    id: root
    property string text: ""
    property string emptyTitle: ""
    property string emptyDetail: ""
    property real codePointSize: Theme.codeSize
    property bool showLineNumbers: true
    property bool syntaxHighlight: false
    property var mappings: []
    property var libraryView: null
    readonly property string displayedText: libraryView ? libraryView.text : text
    readonly property var displayedMappings: libraryView ? libraryView.mappings : mappings
    property string selectedAddress: ""
    function focusContent() { code.forceActiveFocus(Qt.ShortcutFocusReason) }
    signal sourceLineSelected(int line)
    property bool anchorPending: false
    property var appendViewport: null
    property point readingViewport: Qt.point(0, 0)
    function rememberViewport() {
        // TextArea can reset its cursor and scroll before onTextChanged runs.
        // Only record scrolling while the displayed document is stable.
        if (displayedText === code.text && code.text === code.previousText)
            readingViewport = Qt.point(scroll.contentItem.contentX, scroll.contentItem.contentY)
    }
    Connections {
        target: scroll.contentItem
        function onContentXChanged() { root.rememberViewport() }
        function onContentYChanged() { root.rememberViewport() }
    }
    function requestAnchor() {
        anchorPending = true
        Qt.callLater(applyReadingPosition)
    }
    function applyReadingPosition() {
        const viewport = scroll.contentItem
        if (appendViewport !== null) {
            viewport.contentX = appendViewport.x
            viewport.contentY = appendViewport.y
            appendViewport = null
        }
        // Read the latest snapshot here: a queued restore must never retain an
        // address, mapping row or document position from an earlier result.
        if (!anchorPending || displayedText.length === 0 || displayedMappings.length === 0 || selectedAddress.length === 0)
            return
        const position = highlighter.firstMappedPosition()
        if (position < 0)
            return
        code.cursorPosition = position
        if (viewport.width <= 0 || viewport.height <= 0 || code.cursorRectangle.height <= 0)
            return
        const cursor = code.mapToItem(viewport, code.cursorRectangle.x, code.cursorRectangle.y)
        const bottom = cursor.y + code.cursorRectangle.height
        const right = cursor.x + code.cursorRectangle.width
        const dx = cursor.x < 0 ? cursor.x : Math.max(0, right - viewport.width)
        const dy = cursor.y < 0 ? cursor.y : Math.max(0, bottom - viewport.height)
        viewport.contentX = Math.max(0, Math.min(viewport.contentX + dx, viewport.contentWidth - viewport.width))
        viewport.contentY = Math.max(0, Math.min(viewport.contentY + dy, viewport.contentHeight - viewport.height))
        anchorPending = false
    }
    onSelectedAddressChanged: requestAnchor()
    onDisplayedMappingsChanged: if (anchorPending) Qt.callLater(applyReadingPosition)
    color: Theme.editor
    LayoutMirroring.enabled: false
    LayoutMirroring.childrenInherit: true
    // Documents are a bounded, current-function result supplied by the worker.
    // A single native text control preserves text selection, clipboard and IME semantics.
    ScrollView {
        id: scroll
        anchors.fill: parent
        anchors.margins: 0
        visible: root.text.length > 0
        clip: true
        contentWidth: Math.max(availableWidth, code.implicitWidth)
        ScrollBar.vertical.policy: ScrollBar.AsNeeded
        ScrollBar.vertical.active: true
        ScrollBar.vertical.palette.mid: Theme.subdued
        ScrollBar.vertical.palette.dark: Theme.muted
        ScrollBar.horizontal.policy: ScrollBar.AsNeeded
        ScrollBar.horizontal.active: true
        ScrollBar.horizontal.palette.mid: Theme.subdued
        ScrollBar.horizontal.palette.dark: Theme.muted
        TextArea {
            id: code
            objectName: "codeText"
            text: root.displayedText
            readOnly: true
            width: Math.max(scroll.availableWidth, implicitWidth)
            selectByMouse: true
            wrapMode: TextEdit.NoWrap
            textFormat: TextEdit.PlainText
            font.family: Theme.monoFont
            font.pointSize: root.codePointSize
            color: Theme.foreground
            selectionColor: Theme.selection
            selectedTextColor: Theme.accentForeground
            leftPadding: Theme.codeGutter
            rightPadding: 24
            topPadding: 8
            bottomPadding: 12
            background: Rectangle { color: Theme.editor }
            Accessible.name: root.emptyTitle
            property string previousText: ""
            property int previousCursor: 0
            onCursorPositionChanged: if (text === previousText) previousCursor = cursorPosition
            onCursorRectangleChanged: if (root.anchorPending) Qt.callLater(root.applyReadingPosition)
            onTextChanged: {
                const appended = previousText.length > 0 && text.indexOf(previousText) === 0
                if (appended) {
                    if (root.appendViewport === null) {
                        // Qt 6.8 can retain a reference to a point property in
                        // a var. Copy the coordinates before recording changes.
                        root.appendViewport = Qt.point(root.readingViewport.x, root.readingViewport.y)
                    }
                } else {
                    // Replacements retire any deferred append restoration.
                    root.appendViewport = null
                    root.anchorPending = true
                }
                cursorPosition = appended ? Math.min(previousCursor, length) : 0
                previousCursor = cursorPosition
                previousText = text
                root.rememberViewport()
                Qt.callLater(root.applyReadingPosition)
            }
            Keys.onPressed: event => {
                if (root.libraryView && event.matches(StandardKey.Copy)) {
                    root.copySelection()
                    event.accepted = true
                } else if (root.libraryView && (event.key === Qt.Key_Return || event.key === Qt.Key_Enter)) {
                    const region = root.libraryView.regionAt(cursorPosition)
                    if (region.length > 0) {
                        root.libraryView.toggleRegion(region)
                        event.accepted = true
                    }
                }
            }
            NativeCodeHighlighter { id: highlighter; document: code.textDocument; enabled: root.syntaxHighlight; mappings: root.displayedMappings; selectedAddress: root.selectedAddress }
            TapHandler {
                acceptedButtons: Qt.LeftButton
                onSingleTapped: eventPoint => {
                    const position = code.positionAt(eventPoint.position.x, eventPoint.position.y)
                    const region = root.libraryView ? root.libraryView.regionAt(position) : ""
                    if (region.length > 0) root.libraryView.toggleRegion(region)
                    else if (root.displayedMappings.length > 0) root.sourceLineSelected(root.libraryView ? root.libraryView.sourceLineAt(position) : highlighter.lineAtPosition(position))
                }
            }
            TapHandler {
                acceptedButtons: Qt.RightButton
                onSingleTapped: eventPoint => sourceMenu.popup(eventPoint.position.x, eventPoint.position.y)
            }
            Menu {
                id: sourceMenu
                MenuItem { text: qsTr("Copy"); enabled: code.selectionStart !== code.selectionEnd; onTriggered: root.copySelection() }
                MenuItem { text: qsTr("Select all"); onTriggered: code.selectAll() }
            }
        }
    }
    EmptyPane { anchors.fill: parent; visible: root.text.length === 0; title: root.emptyTitle; detail: root.emptyDetail }
    function copySelection() {
        if (libraryView) libraryView.copySelection(code.selectionStart, code.selectionEnd)
        else code.copy()
    }
}
