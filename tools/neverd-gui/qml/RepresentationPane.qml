pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts
import QtQuick.Controls.Basic

Rectangle {
    id: root
    objectName: "representationPane"
    required property var controller
    property real codePointSize: Theme.codeSize
    property var kddockwidgets_min_size: Qt.size(300, 180)
    readonly property var representationIds: ["c", "llvmc", "low", "med", "high", "llvm"]
    readonly property var libraryView: root.controller.libraryView || null
    property bool showLibraryDetails: false
    color: Theme.editor
    function focusContent() { code.focusContent() }
    ColumnLayout {
        anchors.fill: parent
        spacing: 0
        PanelTabs {
            Layout.fillWidth: true
            labels: ["C", "LLVM C", "LowIR", "MedIR", "HighIR", "LLVM IR"]
            currentIndex: Math.max(0, root.representationIds.indexOf(root.controller.representation))
            onSelected: index => root.controller.setRepresentation(root.representationIds[index])
        }
        Rectangle {
            visible: root.controller.loaded
            Layout.fillWidth: true
            Layout.preferredHeight: Theme.controlHeight
            color: Theme.editor
            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 18
                anchors.rightMargin: 6
                Text { textFormat: Text.PlainText; text: root.controller.representationFunctionName || ""; color: root.controller.representationFunctionName ? Theme.functionName : Theme.subdued; font.pointSize: Theme.captionSize; elide: Text.ElideRight; Layout.fillWidth: true; LayoutMirroring.enabled: false }
                WorkbenchButton { text: qsTr("Refresh"); enabled: root.controller.loaded && !root.controller.busy; implicitHeight: Theme.compactControlHeight; onClicked: root.controller.reloadRepresentation() }
                WorkbenchButton { text: root.controller.representationPinned ? qsTr("Unpin") : qsTr("Pin"); hint: qsTr("Keep this function while navigating"); checked: root.controller.representationPinned; enabled: root.controller.loaded; implicitHeight: Theme.compactControlHeight; onClicked: root.controller.toggleRepresentationPin() }
            }
        }
        Rectangle { Layout.fillWidth: true; implicitHeight: 1; color: Theme.border }
        RowLayout {
            visible: !!root.libraryView && root.libraryView.regions.length > 0
            Layout.fillWidth: true
            Layout.leftMargin: 12
            Layout.rightMargin: 6
            Text { text: qsTr("Library operations"); color: Theme.subdued; font.pointSize: Theme.captionSize; Layout.fillWidth: true; elide: Text.ElideRight }
            WorkbenchButton {
                objectName: "foldLibraryButton"
                text: root.libraryView && root.libraryView.anyFolded ? qsTr("Expand all") : qsTr("Fold all")
                enabled: !!root.libraryView && root.libraryView.foldableCount > 0
                hint: qsTr("Fold mapped library operations. Copy and export retain the full source.")
                onClicked: root.libraryView.setFolded(!root.libraryView.anyFolded)
            }
            WorkbenchButton { text: qsTr("Details"); checked: root.showLibraryDetails; onClicked: root.showLibraryDetails = !root.showLibraryDetails }
        }
        TextPane {
            id: code
            Layout.fillWidth: true
            Layout.fillHeight: true
            text: root.controller.representationText
            syntaxHighlight: true
            mappings: root.controller.textMappings
            libraryView: root.libraryView
            selectedAddress: root.controller.selectedAddress
            onSourceLineSelected: line => root.controller.selectTextLine(line)
            codePointSize: root.codePointSize
            emptyTitle: root.controller.loaded ? qsTranslate("Main", "No representation available") : qsTranslate("Main", "Read beyond assembly")
            emptyDetail: root.controller.loaded ? root.controller.representationStatus : qsTranslate("Main", "Compare recovered C, LLVM C, LowIR, MedIR, HighIR, and LLVM IR. Select a function to begin.")
        }
        ColumnLayout {
            id: libraryDetails
            visible: root.showLibraryDetails && !!root.libraryView && root.libraryView.regions.length > 0
            Layout.fillWidth: true
            Layout.margins: 10
            readonly property var region: root.libraryView && regionPicker.currentIndex >= 0 ? root.libraryView.regions[regionPicker.currentIndex] : null
            RowLayout {
                Layout.fillWidth: true
                ComboBox {
                    id: regionPicker
                    objectName: "libraryRegionPicker"
                    Layout.fillWidth: true
                    model: root.libraryView ? root.libraryView.regions : []
                    textRole: "display_name"
                    Accessible.name: qsTr("Recognized library operation")
                }
                WorkbenchButton {
                    text: libraryDetails.region && libraryDetails.region.folded ? qsTr("Expand") : qsTr("Fold")
                    enabled: !!libraryDetails.region && libraryDetails.region.available
                    onClicked: root.libraryView.toggleRegion(libraryDetails.region.id)
                }
                WorkbenchButton { text: qsTr("Close"); onClicked: { root.showLibraryDetails = false; code.focusContent() } }
            }
            TextPane {
                Layout.fillWidth: true
                Layout.preferredHeight: Math.min(180, root.height / 3)
                codePointSize: Theme.captionSize
                emptyTitle: ""
                emptyDetail: ""
                text: {
                    const r = libraryDetails.region
                    if (!r) return ""
                    const addresses = (r.occurrences || []).map(o => o.address + ":" + o.origin_seq).join(", ")
                    return r.display_name + "\n" + (r.available ? qsTr("Mapped to original source") : qsTr("Original source stays expanded; mapping is incomplete or outside the loaded page"))
                        + "\n" + qsTr("Rule") + ": " + r.rule_id + " @ " + r.rule_revision
                        + "\n" + qsTr("Identity evidence") + ": " + r.identity_evidence
                        + "\n" + qsTr("Original instructions") + ": " + addresses
                        + "\n" + qsTr("Linkage") + ": " + (r.linkage_name || "—")
                        + "\n" + qsTr("Pack") + ": " + r.pack_id + "\nSHA-256: " + r.pack_sha256
                        + "\n" + qsTr("Profile SHA-256") + ": " + r.profile_sha256
                        + "\n" + qsTr("Evidence SHA-256") + ": " + r.evidence_sha256
                        + "\n" + qsTr("Source") + ": " + r.source_origin + " @ " + r.source_revision
                }
            }
        }
        RowLayout {
            Layout.fillWidth: true
            Layout.margins: 10
            visible: root.controller.representationStatus.length > 0 && root.controller.representationText.length > 0
            Text { textFormat: Text.PlainText; Layout.fillWidth: true; text: root.controller.representationStatus + " · " + root.controller.mappingStatus; color: Theme.subdued; font.pointSize: Theme.captionSize; wrapMode: Text.WordWrap }
            WorkbenchButton { text: qsTr("Load more lines"); visible: root.controller.hasMoreText; enabled: !root.controller.busy; onClicked: root.controller.loadMoreText() }
        }
    }
}
