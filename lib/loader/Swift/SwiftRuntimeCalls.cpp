#include "neverd/loader/Swift/SwiftRuntimeCalls.h"

#include "../MachO/DarwinRuntimeImport.h"

#include "neverd/ir/SourceABI.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/loader/MachO/DarwinRuntimeCalls.h"

#include "llvm/ADT/StringRef.h"

#include <algorithm>
#include <iterator>

namespace neverd {
namespace {
struct SwiftRuntimeDeclaration {
  const char *Name;
  const char *Signature;
  bool DoesNotReturn;
  bool UsesSwiftConvention;
};
#include "SwiftRuntimeDeclarations.inc"

struct SwiftMetadataDeclaration {
  const char *Name;
  const char *AArch64Modules;
  const char *X64Modules;
};
constexpr SwiftMetadataDeclaration SwiftMetadataDeclarations[] = {
#include "SwiftMetadataDeclarations.inc"
};

struct SwiftSDKDeclaration {
  const char *Name;
  const char *Modules;
  const char *Signature;
  bool DoesNotReturn = false;
};

// Compiler-observed public Foundation bridge entry points. The compact
// signature alphabet records only physical scalar carriers: p is a pointer,
// z is an unsigned word, b is an unsigned byte, I is swift_indirect_result,
// and C is swift_context.
// A parenthesized pair is returned in the two integer result registers.
constexpr SwiftSDKDeclaration SwiftSDKDeclarations[] = {
    // Foundation's NSNotFound getter has no arguments and returns one Int
    // carrier. The exact strong import is required by darwinRuntimeImport.
    {"$s10Foundation10NSNotFoundSivg",
     "/System/Library/Frameworks/Foundation.framework/Foundation|"
     "/System/Library/Frameworks/Foundation.framework/Versions/C/Foundation|"
     "/usr/lib/swift/libswiftFoundation.dylib",
     "z"},
    {"$s10Foundation10URLRequestV19_bridgeToObjectiveCSo12NSURLRequestCyF",
     "/System/Library/Frameworks/Foundation.framework/Foundation|"
     "/System/Library/Frameworks/Foundation.framework/Versions/C/Foundation|"
     "/usr/lib/swift/libswiftFoundation.dylib",
     "pC"},
    {"$s10Foundation10URLRequestV36_unconditionallyBridgeFromObjectiveCyACSo12"
     "NSURLRequestCSgFZ",
     "/System/Library/Frameworks/Foundation.framework/Foundation|"
     "/System/Library/Frameworks/Foundation.framework/Versions/C/Foundation|"
     "/usr/lib/swift/libswiftFoundation.dylib",
     "vIp"},
    {"$s10Foundation12NotificationV19_bridgeToObjectiveCSo14NSNotificationCyF",
     "/System/Library/Frameworks/Foundation.framework/Foundation|"
     "/System/Library/Frameworks/Foundation.framework/Versions/C/Foundation|"
     "/usr/lib/swift/libswiftFoundation.dylib",
     "pC"},
    {"$s10Foundation12NotificationV36_unconditionallyBridgeFromObjectiveCyACSo"
     "14NSNotificationCSgFZ",
     "/System/Library/Frameworks/Foundation.framework/Foundation|"
     "/System/Library/Frameworks/Foundation.framework/Versions/C/Foundation|"
     "/usr/lib/swift/libswiftFoundation.dylib",
     "vIp"},
    {"$s10Foundation13URLComponentsV19_"
     "bridgeToObjectiveCSo15NSURLComponentsCyF",
     "/System/Library/Frameworks/Foundation.framework/Foundation|"
     "/System/Library/Frameworks/Foundation.framework/Versions/C/Foundation|"
     "/usr/lib/swift/libswiftFoundation.dylib",
     "pC"},
    {"$s10Foundation14DateComponentsV36_unconditionallyBridgeFromObjectiveCyAC"
     "So06NSDateC0CSgFZ",
     "/System/Library/Frameworks/Foundation.framework/Foundation|"
     "/System/Library/Frameworks/Foundation.framework/Versions/C/Foundation|"
     "/usr/lib/swift/libswiftFoundation.dylib",
     "vIp"},
    // NSKeyValueObservation.invalidate() passes its receiver in swiftself.
    {"$s10Foundation21NSKeyValueObservationC10invalidateyyFTj",
     "/System/Library/Frameworks/Foundation.framework/Foundation|"
     "/System/Library/Frameworks/Foundation.framework/Versions/C/Foundation|"
     "/usr/lib/swift/libswiftFoundation.dylib",
     "vC"},
    {"$s10Foundation22_convertErrorToNSErrorySo0E0Cs0C0_pF",
     "/System/Library/Frameworks/Foundation.framework/Foundation|"
     "/System/Library/Frameworks/Foundation.framework/Versions/C/Foundation|"
     "/usr/lib/swift/libswiftFoundation.dylib",
     "pp"},
    // Swift 6.1.2 arm64 and x86_64 clients pass Optional<NSError> as i64
    // and receive the Error object as ptr; nil retains its zero word.
    {"$s10Foundation22_convertNSErrorToErrorys0E0_pSo0C0CSgF",
     "/System/Library/Frameworks/Foundation.framework/Foundation|"
     "/System/Library/Frameworks/Foundation.framework/Versions/C/Foundation|"
     "/usr/lib/swift/libswiftFoundation.dylib",
     "pz"},
    // URL.pathExtension reads the URL value through swiftself and returns
    // both words of the String value.
    {"$s10Foundation3URLV13pathExtensionSSvg",
     "/System/Library/Frameworks/Foundation.framework/Foundation|"
     "/System/Library/Frameworks/Foundation.framework/Versions/C/Foundation|"
     "/usr/lib/swift/libswiftFoundation.dylib",
     "(zz)C"},
    // Swift 6.1.2 arm64 and x86_64 clients read URL through swiftself;
    // absoluteString returns both physical String words.
    {"$s10Foundation3URLV14absoluteStringSSvg",
     "/System/Library/Frameworks/Foundation.framework/Foundation|"
     "/System/Library/Frameworks/Foundation.framework/Versions/C/Foundation|"
     "/usr/lib/swift/libswiftFoundation.dylib",
     "(zz)C"},
    {"$s10Foundation3URLV19_bridgeToObjectiveCSo5NSURLCyF",
     "/System/Library/Frameworks/Foundation.framework/Foundation|"
     "/System/Library/Frameworks/Foundation.framework/Versions/C/Foundation|"
     "/usr/lib/swift/libswiftFoundation.dylib",
     "pC"},
    // Swift 6.1.2 arm64 client IR shows URL.appendingPathComponent(String)
    // constructing its URL result through the Swift indirect-result pointer
    // and reading the receiver through swiftself.
    {"$s10Foundation3URLV22appendingPathComponentyACSSF",
     "/System/Library/Frameworks/Foundation.framework/Foundation|"
     "/System/Library/Frameworks/Foundation.framework/Versions/C/Foundation|"
     "/usr/lib/swift/libswiftFoundation.dylib",
     "vIzpC"},
    {"$s10Foundation3URLV36_"
     "unconditionallyBridgeFromObjectiveCyACSo5NSURLCSgFZ",
     "/System/Library/Frameworks/Foundation.framework/Foundation|"
     "/System/Library/Frameworks/Foundation.framework/Versions/C/Foundation|"
     "/usr/lib/swift/libswiftFoundation.dylib",
     "vIp"},
    // Swift 6.1.2 macOS and Mac Catalyst clients on arm64 and x86_64
    // read the opaque URL through swiftself and return both String words.
    {"$s10Foundation3URLV4pathSSvg",
     "/System/Library/Frameworks/Foundation.framework/Foundation|"
     "/System/Library/Frameworks/Foundation.framework/Versions/C/Foundation",
     "(zz)C"},
    // Swift 6.1.2 arm64 client IR passes URL.init(string:) an indirect
    // Optional<URL> result followed by the two physical String words.
    {"$s10Foundation3URLV6stringACSgSSh_tcfC",
     "/System/Library/Frameworks/Foundation.framework/Foundation|"
     "/System/Library/Frameworks/Foundation.framework/Versions/C/Foundation|"
     "/usr/lib/swift/libswiftFoundation.dylib",
     "vIzp"},
    {"$s10Foundation4DataV19_bridgeToObjectiveCSo6NSDataCyF",
     "/System/Library/Frameworks/Foundation.framework/Foundation|"
     "/System/Library/Frameworks/Foundation.framework/Versions/C/Foundation|"
     "/usr/lib/swift/libswiftFoundation.dylib",
     "pzz"},
    {"$s10Foundation4DataV36_"
     "unconditionallyBridgeFromObjectiveCyACSo6NSDataCSgFZ",
     "/System/Library/Frameworks/Foundation.framework/Foundation|"
     "/System/Library/Frameworks/Foundation.framework/Versions/C/Foundation|"
     "/usr/lib/swift/libswiftFoundation.dylib",
     "(zz)p"},
    {"$s10Foundation4DateV19_bridgeToObjectiveCSo6NSDateCyF",
     "/System/Library/Frameworks/Foundation.framework/Foundation|"
     "/System/Library/Frameworks/Foundation.framework/Versions/C/Foundation|"
     "/usr/lib/swift/libswiftFoundation.dylib",
     "pC"},
    {"$s10Foundation4DateV36_"
     "unconditionallyBridgeFromObjectiveCyACSo6NSDateCSgFZ",
     "/System/Library/Frameworks/Foundation.framework/Foundation|"
     "/System/Library/Frameworks/Foundation.framework/Versions/C/Foundation|"
     "/usr/lib/swift/libswiftFoundation.dylib",
     "vIp"},
    // Swift 6.1.2 arm64 and x86_64 UUID.uuidString clients read UUID through
    // swiftself and receive both physical String words in result registers.
    {"$s10Foundation4UUIDV10uuidStringSSvg",
     "/System/Library/Frameworks/Foundation.framework/Foundation|"
     "/System/Library/Frameworks/Foundation.framework/Versions/C/Foundation|"
     "/usr/lib/swift/libswiftFoundation.dylib",
     "(zz)C"},
    // UUID.init() constructs the opaque UUID through swift_indirect_result
    // on both Darwin targets; there are no ordinary or context inputs.
    {"$s10Foundation4UUIDVACycfC",
     "/System/Library/Frameworks/Foundation.framework/Foundation|"
     "/System/Library/Frameworks/Foundation.framework/Versions/C/Foundation|"
     "/usr/lib/swift/libswiftFoundation.dylib",
     "vI"},
    // Locale.preferredLanguages returns the Array object in one register.
    {"$s10Foundation6LocaleV18preferredLanguagesSaySSGvgZ",
     "/System/Library/Frameworks/Foundation.framework/Foundation|"
     "/System/Library/Frameworks/Foundation.framework/Versions/C/Foundation|"
     "/usr/lib/swift/libswiftFoundation.dylib",
     "p"},
    {"$s10Foundation6LocaleV19_bridgeToObjectiveCSo8NSLocaleCyF",
     "/System/Library/Frameworks/Foundation.framework/Foundation|"
     "/System/Library/Frameworks/Foundation.framework/Versions/C/Foundation|"
     "/usr/lib/swift/libswiftFoundation.dylib",
     "pC"},
    {"$s10Foundation6LocaleV36_"
     "unconditionallyBridgeFromObjectiveCyACSo8NSLocale"
     "CSgFZ",
     "/System/Library/Frameworks/Foundation.framework/Foundation|"
     "/System/Library/Frameworks/Foundation.framework/Versions/C/Foundation|"
     "/usr/lib/swift/libswiftFoundation.dylib",
     "vIp"},
    // Swift 6.1.2 arm64 and x86_64 clients return Calendar.current through
    // the indirect-result carrier, with no ordinary or context parameters.
    {"$s10Foundation8CalendarV7currentACvgZ",
     "/System/Library/Frameworks/Foundation.framework/Foundation|"
     "/System/Library/Frameworks/Foundation.framework/Versions/C/Foundation|"
     "/usr/lib/swift/libswiftFoundation.dylib",
     "vI"},
    {"$s10Foundation9IndexPathV19_bridgeToObjectiveCSo07NSIndexC0CyF",
     "/System/Library/Frameworks/Foundation.framework/Foundation|"
     "/System/Library/Frameworks/Foundation.framework/Versions/C/Foundation|"
     "/usr/lib/swift/libswiftFoundation.dylib",
     "pC"},
    {"$s10Foundation9IndexPathV36_unconditionallyBridgeFromObjectiveCyACSo07NS"
     "IndexC0CSgFZ",
     "/System/Library/Frameworks/Foundation.framework/Foundation|"
     "/System/Library/Frameworks/Foundation.framework/Versions/C/Foundation|"
     "/usr/lib/swift/libswiftFoundation.dylib",
     "vIp"},
    // Swift 6.1.2 macOS/Mac Catalyst clients on arm64 and x86_64 pass
    // the inout Set address first and AnyCancellable through swiftself.
    {"$s7Combine14AnyCancellableC5store2inyShyACGz_tF",
     "/System/Library/Frameworks/Combine.framework/Combine|"
     "/System/Library/Frameworks/Combine.framework/Versions/A/Combine",
     "vpC"},
    // Swift 6.1.2 macOS and Mac Catalyst client IR on arm64 and x86_64
    // declares the initializing constructor as swiftcc ptr (ptr value,
    // ptr swiftself). The opaque consumed value is not a by-value word;
    // the context is the allocated instance, not an extra metadata argument.
    {"$s7Combine19CurrentValueSubjectCyACyxq_Gxcfc",
     "/System/Library/Frameworks/Combine.framework/Combine|"
     "/System/Library/Frameworks/Combine.framework/Versions/A/Combine",
     "ppC"},
    // Swift 6.1.2 arm64 and x86_64 client IR declares Published.init as
    // swiftcc void (ptr sret, ptr value, ptr genericMetadata). The result and
    // consumed input stay opaque; neither carrier uses swiftself.
    {"$s7Combine9PublishedV12initialValueACyxGx_tcfC",
     "/System/Library/Frameworks/Combine.framework/Combine|"
     "/System/Library/Frameworks/Combine.framework/Versions/A/Combine",
     "vIpp"},
    // The Never-failing Publisher sink overload receives closure code and
    // context, Publisher metadata and its witness table, then the opaque
    // borrowed Publisher address in swiftself. The result is AnyCancellable.
    // All five pointer carriers are compiler-observed on the same four SDK
    // targets; this declaration grants no callback or frame escape effects.
    {"$s7Combine9PublisherPAAs5NeverO7FailureRtzrlE4sink12receiveValueAA14"
     "AnyCancellableCy6OutputQzc_tF",
     "/System/Library/Frameworks/Combine.framework/Combine|"
     "/System/Library/Frameworks/Combine.framework/Versions/A/Combine",
     "pppppC"},
    // Binding.wrappedValue's generic setter receives the value address,
    // Binding metadata, and the mutable Binding in swiftself.
    {"$s7SwiftUI7BindingV12wrappedValuexvs",
     "/System/Library/Frameworks/SwiftUI.framework/SwiftUI", "vppC"},
    // Swift 6.1.2 arm64 and x86_64 clients construct the opaque QoS value
    // through the Swift indirect-result pointer.
    {"$s8Dispatch0A3QoSV11unspecifiedACvgZ",
     "/usr/lib/swift/libswiftDispatch.dylib", "vI"},
    // Swift 6.1.2 arm64 and x86_64 client IR return the opaque DispatchTime
    // value through the Swift indirect-result pointer.
    {"$s8Dispatch0A4TimeV3nowACyFZ", "/usr/lib/swift/libswiftDispatch.dylib",
     "vI"},
    // Swift 6.1.2 arm64 and x86_64 client IR declare DispatchWorkItem.cancel
    // as swiftcc void (ptr swiftself).
    {"$s8Dispatch0A8WorkItemC6cancelyyFTj",
     "/usr/lib/swift/libswiftDispatch.dylib", "vC"},
    // DispatchTime + DispatchTimeInterval takes both opaque values by address
    // and writes its result through the Swift indirect-result pointer.
    {"$s8Dispatch1poiyAA0A4TimeVAD_AA0aB8IntervalOtF",
     "/usr/lib/swift/libswiftDispatch.dylib", "vIpp"},
    {"$sSD10FoundationE19_bridgeToObjectiveCSo12NSDictionaryCyF",
     "/System/Library/Frameworks/Foundation.framework/Foundation|"
     "/System/Library/Frameworks/Foundation.framework/Versions/C/Foundation|"
     "/usr/lib/swift/libswiftFoundation.dylib",
     "ppppp"},
    {"$sSD10FoundationE36_unconditionallyBridgeFromObjectiveCySDyxq_"
     "GSo12NSDictionaryCSgFZ",
     "/System/Library/Frameworks/Foundation.framework/Foundation|"
     "/System/Library/Frameworks/Foundation.framework/Versions/C/Foundation|"
     "/usr/lib/swift/libswiftFoundation.dylib",
     "ppppp"},
    // Dictionary.count takes the storage pointer, Key and Value metadata,
    // then Key's Hashable witness. Swift 6.1.2 arm64 and x86_64 client IR
    // uses four ordinary pointer carriers and returns one integer word.
    {"$sSD5countSivg", "/usr/lib/swift/libswiftCore.dylib", "zpppp"},
    // Swift 6.1.2 arm64 and x86_64 clients pass the borrowed String words
    // and Array<CVarArg> object as three ordinary carriers, with no context.
    {"$sSS10FoundationE6format9argumentsS2Sh_Says7CVarArg_pGhtcfC",
     "/System/Library/Frameworks/Foundation.framework/Foundation|"
     "/System/Library/Frameworks/Foundation.framework/Versions/C/Foundation|"
     "/usr/lib/swift/libswiftFoundation.dylib",
     "(zz)zpp"},
    {"$sSS10lowercasedSSyF", "/usr/lib/swift/libswiftCore.dylib", "(zz)zp"},
    // Swift 6.1.2 arm64/x86_64 clients at Onone and O pass the two String
    // words as ordinary carriers and return the ContiguousArray storage.
    {"$sSS11utf8CStrings15ContiguousArrayVys4Int8VGvg",
     "/usr/lib/swift/libswiftCore.dylib", "pzp"},
    // The capacity is an Int carrier; the mutable String's address is
    // swiftself in both arm64 and x86_64 Swift 6.1.2 client IR.
    {"$sSS15reserveCapacityyySiF", "/usr/lib/swift/libswiftCore.dylib", "vzC"},
    // Swift 6.1.2 arm64 client IR passes the inout Hasher address followed
    // by the two String words to String.hash(into:).
    {"$sSS4hash4intoys6HasherVz_tF", "/usr/lib/swift/libswiftCore.dylib",
     "vpzp"},
    // The same four SDK profiles lower String.count to swiftcc i64(i64, ptr).
    // Both borrowed String words remain ordinary arguments, not swiftself.
    {"$sSS5countSivg", "/usr/lib/swift/libswiftCore.dylib", "zzp"},
    {"$sSS5index5afterSS5IndexVAD_tF", "/usr/lib/swift/libswiftCore.dylib",
     "zzzp"},
    // Swift String is passed as its two scalar carriers; the mutable
    // destination is the swiftself pointer.
    {"$sSS6appendyySSF", "/usr/lib/swift/libswiftCore.dylib", "vzpC"},
    // Character occupies the same two scalar result carriers as String.
    {"$sSSySJSS5IndexVcig", "/usr/lib/swift/libswiftCore.dylib", "(zz)zzp"},
    // Swift 6.1.2 arm64 and x86_64 client IR specializes Array<AnyObject>
    // append into these exact libswiftCore entries. The mutable Array value
    // lives in swiftself; the element is an object pointer.
    {"$sSa034_makeUniqueAndReserveCapacityIfNotB0yyFyXl_Ts5",
     "/usr/lib/swift/libswiftCore.dylib", "vC"},
    {"$sSa10FoundationE19_bridgeToObjectiveCSo7NSArrayCyF",
     "/System/Library/Frameworks/Foundation.framework/Foundation|"
     "/System/Library/Frameworks/Foundation.framework/Versions/C/Foundation|"
     "/usr/lib/swift/libswiftFoundation.dylib",
     "ppp"},
    {"$sSa10FoundationE36_unconditionallyBridgeFromObjectiveCySayxGSo7NSArrayC"
     "SgFZ",
     "/System/Library/Frameworks/Foundation.framework/Foundation|"
     "/System/Library/Frameworks/Foundation.framework/Versions/C/Foundation|"
     "/usr/lib/swift/libswiftFoundation.dylib",
     "ppp"},
    {"$sSa16_createNewBuffer14bufferIsUnique15minimumCapacity13growForAppendy"
     "Sb_SiSbtFyXl_Ts5",
     "/usr/lib/swift/libswiftCore.dylib", "vbzbC"},
    {"$sSa37_appendElementAssumeUniqueAndCapacity_03newB0ySi_xntFyXl_Ts5",
     "/usr/lib/swift/libswiftCore.dylib", "vzpC"},
    // DispatchQueue.global(qos:) reads the QoSClass value by address and
    // receives the queue metatype in swiftself on both Darwin targets.
    {"$sSo17OS_dispatch_queueC8DispatchE6global3qosAbC0D3QoSV0G6SClassO_tFZ",
     "/usr/lib/swift/libswiftDispatch.dylib", "ppC"},
    // DispatchSource.makeTimerSource takes TimerFlags by address, a queue
    // optional in one integer carrier, and the source metatype in swiftself.
    {"$sSo18OS_dispatch_sourceC8DispatchE15makeTimerSource5flags5queueSo0a1_"
     "b1_C6_timer_pAbCE0F5FlagsV_So0a1_b1_I0CSgtFZ",
     "/usr/lib/swift/libswiftDispatch.dylib", "ppzC"},
    // The event-handler overload receives QoS and flags by address, the
    // Objective-C block pointer bits, dynamic source type, and swiftself.
    {"$sSo18OS_dispatch_sourceP8DispatchE15setEventHandler3qos5flags7handler"
     "yAC0D3QoSV_AC0D13WorkItemFlagsVyyXBSgtF",
     "/usr/lib/swift/libswiftDispatch.dylib", "vppzpC"},
    // DispatchSourceProtocol.resume/suspend receive the dynamic source type
    // in the first ordinary argument and the source object in swiftself.
    // Swift's optimized arm64 and x86_64 IR declare both exact overlay
    // entries as swiftcc void (ptr, ptr swiftself).
    {"$sSo18OS_dispatch_sourceP8DispatchE6resumeyyF",
     "/usr/lib/swift/libswiftDispatch.dylib", "vpC"},
    {"$sSo18OS_dispatch_sourceP8DispatchE7suspendyyF",
     "/usr/lib/swift/libswiftDispatch.dylib", "vpC"},
    {"$sSo21OS_dispatch_semaphoreC8DispatchE4waityyF",
     "/usr/lib/swift/libswiftDispatch.dylib", "vC"},
    {"$sSo21OS_dispatch_semaphoreC8DispatchE6signalSiyF",
     "/usr/lib/swift/libswiftDispatch.dylib", "zC"},
    // DispatchSourceTimer.schedule receives three value addresses, dynamic
    // source metadata, and the source object in swiftself.
    {"$sSo24OS_dispatch_source_timerP8DispatchE8schedule8deadline9repeating6"
     "leewayyAC0E4TimeV_AC0eJ8IntervalOAKtF",
     "/usr/lib/swift/libswiftDispatch.dylib", "vppppC"},
    // Swift 6.1.2 arm64 and x86_64 Foundation clients preserve the generic
    // class metadata separately from the class argument and swiftself. The
    // typed Optional object is one integer word; Any? is an indirect result.
    {"$sSo7NSCoderC10FoundationE12decodeObject2of6forKeyxSgxm_"
     "SStSo8NSObjectCRbzSo8NSCodingRzlF",
     "/System/Library/Frameworks/Foundation.framework/Foundation|"
     "/System/Library/Frameworks/Foundation.framework/Versions/C/Foundation|"
     "/usr/lib/swift/libswiftFoundation.dylib",
     "zpzppC"},
    {"$sSo7NSCoderC10FoundationE12decodeObject2of6forKeyypSgSayyXlXpGSg_SStF",
     "/System/Library/Frameworks/Foundation.framework/Foundation|"
     "/System/Library/Frameworks/Foundation.framework/Versions/C/Foundation|"
     "/usr/lib/swift/libswiftFoundation.dylib",
     "vIzzpC"},
    // The concrete UIImage initializer consumes the two String words in x0/x1
    // and returns an object in x0. WMF's arm64 call uses those carriers and
    // links the exact Swift overlay symbol from UIKit.
    // https://developer.apple.com/documentation/uikit/uiimage/init(imageliteralresourcename:)
    {"$sSo7UIImageC5UIKitE24imageLiteralResourceNameABSS_tcfC",
     "/System/Library/Frameworks/UIKit.framework/UIKit", "pzp"},
    // NSNumber(integerLiteral:) takes the integer in the first argument
    // register and the NSNumber metatype in swiftself.
    {"$sSo8NSNumberC10FoundationE14integerLiteralABSi_tcfC",
     "/System/Library/Frameworks/Foundation.framework/Foundation|"
     "/System/Library/Frameworks/Foundation.framework/Versions/C/Foundation|"
     "/usr/lib/swift/libswiftFoundation.dylib",
     "pzC"},
    // StringProtocol.caseInsensitiveCompare<String> carries five generic
    // pointers and the String value address in swiftself.
    {"$sSy10FoundationE22caseInsensitiveCompareySo18NSComparisonResultVqd__"
     "SyRd__lF",
     "/System/Library/Frameworks/Foundation.framework/Foundation|"
     "/System/Library/Frameworks/Foundation.framework/Versions/C/Foundation|"
     "/usr/lib/swift/libswiftFoundation.dylib",
     "zpppppC"},
    // Swift 6.1.2 emits StringProtocol.contains<String> as five ordinary
    // pointer carriers plus the haystack value in swiftself. The generic
    // conformance and metadata arguments remain explicit runtime inputs.
    {"$sSy10FoundationE8containsySbqd__SyRd__lF",
     "/System/Library/Frameworks/Foundation.framework/Foundation|"
     "/System/Library/Frameworks/Foundation.framework/Versions/C/Foundation|"
     "/usr/lib/swift/libswiftFoundation.dylib",
     "bpppppC"},
    {"$ss018_bridgeAnyObjectToB0yypyXlSgF", "/usr/lib/swift/libswiftCore.dylib",
     "vIp"},
    // SetAlgebra.init<S: Sequence>(_:) carries an indirect result and
    // sequence address, followed by generic metadata and witnesses. Swift
    // 6.1.2 arm64 and x86_64 clients put T metadata in swiftself.
    {"$ss10SetAlgebraPyxqd__ncSTRd__7ElementQyd__ACRtzlufCTj",
     "/usr/lib/swift/libswiftCore.dylib", "vIpppCpp"},
    // The mutating _StringGuts.grow(Int) entry takes the capacity in the
    // first integer register and the two-word guts address in swiftself.
    {"$ss11_StringGutsV4growyySiF", "/usr/lib/swift/libswiftCore.dylib", "vzC"},
    // Both targets pass StaticString as (i64, i64, i8) and return both String
    // words. Its representation flags are a byte, not a canonical Boolean.
    {"$ss12StaticStringV11descriptionSSvg", "/usr/lib/swift/libswiftCore.dylib",
     "(zz)zzb"},
    // Swift 6.1.2 arm64 and x86_64 Array<AnyObject> subscript clients
    // declare this exact specialization as swiftcc ptr (i64, ptr). The
    // index precedes the buffer value; neither input uses swiftself.
    {"$ss12_ArrayBufferV19_getElementSlowPathyyXlSiFyXl_Ts5",
     "/usr/lib/swift/libswiftCore.dylib", "pzp"},
    // Swift 6.1.2 arm64 and x86_64 clients declare swiftcc void
    // (i1 isUnique, ptr concreteMetadata, ptr swiftself dictionaryAddress).
    // Publication must prove a canonical Boolean before using a byte carrier.
    {"$ss17_NativeDictionaryV9removeAll8isUniqueySb_tF",
     "/usr/lib/swift/libswiftCore.dylib", "vbpC"},
    {"$ss18_CocoaArrayWrapperV8endIndexSivg",
     "/usr/lib/swift/libswiftCore.dylib", "zz"},
    // Swift 6.1.2 DictionaryStorage.swift defines the original storage,
    // capacity and move flag; the specialized generic metadata is swiftself.
    {"$ss18_DictionaryStorageC4copy8originalAByxq_Gs05__RawaB0C_tFZ",
     "/usr/lib/swift/libswiftCore.dylib", "ppC"},
    {"$ss18_DictionaryStorageC6resize8original8capacity4moveAByxq_Gs05__"
     "RawaB0C_SiSbtFZ",
     "/usr/lib/swift/libswiftCore.dylib", "ppzbC"},
    // Swift 6.1.2 optimized arm64 and x86_64 client IR passes capacity as
    // an Int and concrete dictionary metadata through swiftself.
    {"$ss18_DictionaryStorageC8allocate8capacityAByxq_GSi_tFZ",
     "/usr/lib/swift/libswiftCore.dylib", "pzC"},
    {"$ss27_bridgeAnythingToObjectiveCyyXlxlF",
     "/usr/lib/swift/libswiftCore.dylib", "ppp"},
    // Swift 6.1.2 NativeDictionary.swift declares this exact diagnostic
    // with one Any.Type input and a Never result. It must retain its trap.
    {"$ss53KEY_TYPE_OF_DICTIONARY_VIOLATES_HASHABLE_REQUIREMENTSys5NeverOypXpF",
     "/usr/lib/swift/libswiftCore.dylib", "vp", true},
    // The Hasher's 72-byte value is returned through x8; the dictionary
    // caller passes its seed in x0, then _finalize reads the value in x20.
    {"$ss6HasherV5_seedABSi_tcfC", "/usr/lib/swift/libswiftCore.dylib", "vIz"},
    {"$ss6HasherV9_finalizeSiyF", "/usr/lib/swift/libswiftCore.dylib", "zC"},
    // Swift 6.1.2 arm64 and x86_64 client IR passes an ordinary metadata
    // pointer and a Boolean qualifier; the String result occupies both
    // integer return carriers. Neither input is swiftself.
    {"$ss9_typeName_9qualifiedSSypXp_SbtF", "/usr/lib/swift/libswiftCore.dylib",
     "(zz)pb"},
};

bool declaredSDKABI(const BinaryImage &Image, va_t Slot,
                    SourceCallTypeHint &Hint) {
  const auto Found = std::lower_bound(
      std::begin(SwiftSDKDeclarations), std::end(SwiftSDKDeclarations),
      Hint.TargetName, [](const auto &Row, llvm::StringRef Name) {
        return llvm::StringRef(Row.Name) < Name;
      });
  const auto Bind = Image.DyldBindSlots.find(Slot);
  if (Found == std::end(SwiftSDKDeclarations) ||
      Hint.TargetName != Found->Name || Bind == Image.DyldBindSlots.end() ||
      !darwinExportModuleMatches(Found->Modules, Bind->second.Module))
    return false;

  const auto Word = NdType::makeInt(8, false);
  const auto Byte = NdType::makeInt(1, false);
  const auto Pointer = NdType::makePtr(NdType::makeVoid());
  auto &Signature = Hint.Signature;
  Signature.Origin = SourceFunctionTypeHint::OriginKind::SwiftSDK;
  llvm::StringRef Encoding(Found->Signature);
  if (Encoding.consume_front("(zz)"))
    Signature.ReturnType = NdType::makeInt(16, false);
  else if (Encoding.consume_front("p"))
    Signature.ReturnType = Pointer;
  else if (Encoding.consume_front("z"))
    Signature.ReturnType = Word;
  else if (Encoding.consume_front("b"))
    Signature.ReturnType = Byte;
  else if (Encoding.consume_front("v"))
    Signature.ReturnType = NdType::makeVoid();
  else
    return false;
  if (Found->DoesNotReturn && Signature.ReturnType->Kind != NdTypeKind::Void)
    return false;
  Hint.DoesNotReturn = Found->DoesNotReturn;
  if (Hint.TargetName == "$ss9_typeName_9qualifiedSSypXp_SbtF")
    Hint.CanonicalBooleanInputs = {1};
  if (Hint.TargetName == "$ss17_NativeDictionaryV9removeAll8isUniqueySb_tF")
    Hint.CanonicalBooleanInputs = {0};
  if (Hint.TargetName == "$ss12StaticStringV11descriptionSSvg")
    Hint.SwiftStaticStringInputs = {{0, 1, 2}};
  if (Hint.TargetName ==
      "$sSS10FoundationE6format9argumentsS2Sh_Says7CVarArg_pGhtcfC")
    Hint.SwiftStringInputs = {{0, 1}};
  // UIKit's image-literal initializer receives the opaque String words in
  // x0/x1. Authenticate that exact SDK import before allowing its immutable
  // literal storage to be copied into the generated source.
  if (Hint.TargetName ==
      "$sSo7UIImageC5UIKitE24imageLiteralResourceNameABSS_tcfC")
    Hint.SwiftStringInputs = {{0, 1}};
  if (Hint.TargetName == "$s10Foundation3URLV6stringACSgSSh_tcfC")
    Hint.SwiftStringInputs = {{1, 2}};
  if (Hint.TargetName ==
      "$sSo7NSCoderC10FoundationE12decodeObject2of6forKeyxSgxm_"
      "SStSo8NSObjectCRbzSo8NSCodingRzlF")
    Hint.SwiftStringInputs = {{1, 2}};
  if (Hint.TargetName ==
      "$sSo7NSCoderC10FoundationE12decodeObject2of6forKeyypSgSayyXlXpGSg_SStF")
    Hint.SwiftStringInputs = {{2, 3}};
  for (char Code : Encoding) {
    SourceParameterTypeHint Parameter;
    Parameter.Name = "arg" + std::to_string(Signature.Parameters.size());
    Parameter.Type = Code == 'z' ? Word : Code == 'b' ? Byte : Pointer;
    if (Code == 'I')
      Parameter.TheRole = SourceParameterTypeHint::Role::SwiftIndirectResult;
    else if (Code == 'C')
      Parameter.TheRole = SourceParameterTypeHint::Role::SwiftContext;
    else if (Code != 'p' && Code != 'z' && Code != 'b')
      return false;
    Signature.Parameters.push_back(std::move(Parameter));
  }
  std::string Diagnostic;
  return assignDarwinSwiftSourceABI(Signature, Image.Arch, Diagnostic);
}

bool declaredMetadataABI(const BinaryImage &Image, va_t Slot,
                         SourceCallTypeHint &Hint) {
  const auto Found =
      std::lower_bound(std::begin(SwiftMetadataDeclarations),
                       std::end(SwiftMetadataDeclarations), Hint.TargetName,
                       [](const auto &Row, llvm::StringRef Name) {
                         return llvm::StringRef(Row.Name) < Name;
                       });
  const auto Bind = Image.DyldBindSlots.find(Slot);
  if (Found == std::end(SwiftMetadataDeclarations) ||
      Hint.TargetName != Found->Name || Bind == Image.DyldBindSlots.end() ||
      !darwinExportModuleMatches(Image.Arch == Arch::AArch64
                                     ? Found->AArch64Modules
                                     : Found->X64Modules,
                                 Bind->second.Module))
    return false;
  auto &Signature = Hint.Signature;
  Signature.Origin = SourceFunctionTypeHint::OriginKind::SwiftSDK;
  Signature.ReturnType = NdType::makeStruct(
      {NdType::makePtr(NdType::makeVoid()), NdType::makeInt(8, false)});
  Signature.Parameters = {{"request", NdType::makeInt(8, false)}};
  std::string Diagnostic;
  return assignDarwinSwiftSourceABI(Signature, Image.Arch, Diagnostic);
}

bool declaredStdlibABI(const BinaryImage &Image, va_t Slot,
                       llvm::StringRef Name, SourceCallTypeHint &Hint) {
  constexpr llvm::StringLiteral AssertionFailure =
      "$ss17_assertionFailure__4file4line5flagss5NeverOs12StaticStringV_"
      "SSAHSus6UInt32VtF";
  constexpr llvm::StringLiteral StringRangeSubscript =
      "$sSSySsSnySS5IndexVGcig";
  constexpr llvm::StringLiteral AllocError = "swift_allocError";
  constexpr llvm::StringLiteral OpaqueConformance2 =
      "swift_getOpaqueTypeConformance2";
  constexpr llvm::StringLiteral DynamicType = "swift_getDynamicType";
  const auto Bind = Image.DyldBindSlots.find(Slot);
  if (Bind == Image.DyldBindSlots.end() ||
      Bind->second.Module != "/usr/lib/swift/libswiftCore.dylib" ||
      (Name != AssertionFailure && Name != StringRangeSubscript &&
       Name != AllocError && Name != OpaqueConformance2 &&
       Name != DynamicType) ||
      ((Name == StringRangeSubscript || Name == AllocError ||
        Name == OpaqueConformance2) &&
       Image.Arch != Arch::AArch64))
    return false;

  auto &Signature = Hint.Signature;
  Signature.Origin = SourceFunctionTypeHint::OriginKind::SwiftSDK;
  const auto Word = NdType::makeInt(8, false);
  const auto Pointer = NdType::makePtr(NdType::makeVoid());
  if (Name == DynamicType) {
    // Compiler IR on both Darwin targets declares a C-convention pointer
    // result from (value address, static metadata, Boolean existential flag).
    // i1 alone does not declare a complete uint8_t parameter. Publication
    // requires this flag to be exactly zero or one, whose full byte/32-bit
    // extension agrees with the observed callers on both architectures.
    Hint.CanonicalBooleanInputs = {2};
    Signature.ReturnType = Pointer;
    Signature.Parameters = {{"value", Pointer},
                            {"static_type", Pointer},
                            {"existential", NdType::makeInt(1, false)}};
    std::string Diagnostic;
    return assignDarwinScalarSourceABI(Signature, Image.Arch, Diagnostic);
  }
  if (Name == AllocError) {
    // Swift 6.1.2 arm64 client IR: swiftcc { ptr, ptr }
    // (ptr metadata, ptr witness, ptr initialValue, i1 isTake). Its generated
    // assembly stores the error payload through the second result in x1.
    Signature.ReturnType = NdType::makeStruct({Pointer, Pointer});
    Signature.Parameters = {{"type", Pointer},
                            {"conformance", Pointer},
                            {"initial_value", Pointer},
                            {"is_take", NdType::makeInt(1, false)}};
    std::string Diagnostic;
    return assignDarwinSwiftSourceABI(Signature, Image.Arch, Diagnostic);
  }
  if (Name == OpaqueConformance2) {
    // Swift RuntimeFunctions.def (9215272a) declares a swiftcc witness-table
    // pointer result from (arguments, signed descriptor, index). The strong
    // libswiftCore import proves this versioned entry is present.
    Signature.ReturnType = Pointer;
    Signature.Parameters = {
        {"arguments", Pointer}, {"descriptor", Pointer}, {"index", Word}};
    std::string Diagnostic;
    return assignDarwinSwiftSourceABI(Signature, Image.Arch, Diagnostic);
  }
  if (Name == StringRangeSubscript) {
    // Swift 6.1.2 arm64 client IR: swiftcc { i64, i64, i64, ptr }
    // (i64, i64, i64, ptr). The range precedes the String value; neither
    // its four-word result nor its storage can be collapsed to a pointer.
    Signature.ReturnType = NdType::makeStruct({Word, Word, Word, Pointer});
    Signature.Parameters = {{"lower", Word},
                            {"upper", Word},
                            {"string_bits", Word},
                            {"string_storage", Pointer}};
    Hint.SwiftStringInputs = {{2, 3}};
    std::string Diagnostic;
    return assignDarwinSwiftSourceABI(Signature, Image.Arch, Diagnostic);
  }
  const auto Byte = NdType::makeInt(1, false);
  Signature.ReturnType = NdType::makeVoid();
  // Compiler IR lowers the two StaticString values to (i64, i64, i8), the
  // String value to (i64, ptr), followed by UInt and UInt32. This is a fixed
  // transport declaration; it does not expose or reconstruct either layout.
  Signature.Parameters = {{"message_address", Word},
                          {"message_count", Word},
                          {"message_flags", Byte},
                          {"detail_bits", Word},
                          {"detail_storage", Pointer},
                          {"file_address", Word},
                          {"file_count", Word},
                          {"file_flags", Byte},
                          {"line", Word},
                          {"flags", NdType::makeInt(4, false)}};
  // StaticString's data word is a pointer only when its flags say so.
  // Rebuild bytes only for that representation; retain scalar code points.
  Hint.SwiftStaticStringInputs = {{0, 1, 2}, {5, 6, 7}};
  Hint.SwiftStringInputs = {{3, 4}};
  Hint.DoesNotReturn = true;
  std::string Diagnostic;
  return assignDarwinSwiftSourceABI(Signature, Image.Arch, Diagnostic);
}

bool declaredFixedABI(const BinaryImage &Image, va_t Slot, llvm::StringRef Name,
                      SourceCallTypeHint &Hint) {
  const auto *Found = std::lower_bound(
      std::begin(SwiftRuntimeDeclarations), std::end(SwiftRuntimeDeclarations),
      Name, [](const SwiftRuntimeDeclaration &Row, llvm::StringRef Value) {
        return llvm::StringRef(Row.Name) < Value;
      });
  if (Found == std::end(SwiftRuntimeDeclarations) || Name != Found->Name)
    return false;
  if (Found->UsesSwiftConvention) {
    // Versioned runtime entries carry the same fixed ABI when strongly
    // imported from their declared provider. A weak or absent import cannot
    // establish availability; a same-named user function supplies no evidence.
    const auto Bind = Image.DyldBindSlots.find(Slot);
    if (Bind == Image.DyldBindSlots.end() ||
        Bind->second.Module != "/usr/lib/swift/libswiftCore.dylib")
      return false;
  }
  const auto Type = [](char Encoding) -> TypeRef {
    switch (Encoding) {
    case 'v':
      return NdType::makeVoid();
    case 'p':
      return NdType::makePtr(NdType::makeVoid());
    case 'z':
      return NdType::makeInt(8, false);
    case 'u':
      return NdType::makeInt(4, false);
    case 'b':
      // Catalog boolean results have an explicit zero-extension contract.
      // Keep their complete low byte as an unsigned source carrier.
      return NdType::makeInt(1, false);
    default:
      return {};
    }
  };
  llvm::StringRef Encoding(Found->Signature);
  if (Encoding.empty() || Encoding.size() > 20)
    return false;
  auto &Signature = Hint.Signature;
  if (Encoding.consume_front("(")) {
    // The catalog records only complete, explicitly declared two-word
    // Swift results. Their physical carriers belong to the shared ABI layer.
    if (!Found->UsesSwiftConvention || Encoding.size() < 3 ||
        Encoding[2] != ')' || (Encoding[0] != 'p' && Encoding[0] != 'z') ||
        (Encoding[1] != 'p' && Encoding[1] != 'z'))
      return false;
    Signature.ReturnType =
        NdType::makeStruct({Type(Encoding[0]), Type(Encoding[1])});
    Encoding = Encoding.drop_front(3);
  } else {
    Signature.ReturnType = Type(Encoding.front());
    Encoding = Encoding.drop_front();
  }
  if (!Signature.ReturnType)
    return false;
  Signature.Parameters.clear();
  for (char Code : Encoding) {
    const auto Parameter = Type(Code);
    if (!Parameter || Parameter->Kind == NdTypeKind::Void)
      return false;
    Signature.Parameters.push_back(
        {"arg" + std::to_string(Signature.Parameters.size()), Parameter});
  }
  Signature.Convention = Found->UsesSwiftConvention
                             ? SourceFunctionTypeHint::ConventionKind::Swift
                             : SourceFunctionTypeHint::ConventionKind::C;
  Hint.DoesNotReturn = Found->DoesNotReturn;
  return !Hint.DoesNotReturn || Signature.ReturnType->Kind == NdTypeKind::Void;
}
} // namespace

bool swiftWitnessInstantiationArgumentUnused(const BinaryImage &Image,
                                             va_t DescriptorSlot) {
  // The data owner proves a strong, exact, non-TLS external address. This
  // separate compiler catalog proves argument irrelevance for every legal
  // generic instantiation; the ordinary runtime ABI still has three pointers.
  const auto Data = darwinRuntimeGlobalAddressHint(Image, DescriptorSlot);
  const auto Bind = Image.DyldBindSlots.find(DescriptorSlot);
  if (!Data ||
      Data->Signature.Origin !=
          SourceFunctionTypeHint::OriginKind::SwiftRuntime ||
      Data->WeakImport || Bind == Image.DyldBindSlots.end() ||
      Bind->second.WeakImport || Bind->second.Addend ||
      Bind->second.Name != "_" + Data->TargetName)
    return false;
  constexpr SwiftMetadataDeclaration Contracts[] = {
#include "SwiftWitnessContracts.inc"
  };
  for (const auto &Contract : Contracts)
    if (Data->TargetName == Contract.Name &&
        darwinExportModuleMatches(Image.Arch == Arch::AArch64
                                      ? Contract.AArch64Modules
                                      : Contract.X64Modules,
                                  Bind->second.Module))
      return true;
  return false;
}

std::optional<SourceCallTypeHint>
swiftRuntimeSourceCallHint(const BinaryImage &Image, va_t ImportSlot) {
  const auto Import = darwinRuntimeImport(Image, ImportSlot);
  if (!Import)
    return std::nullopt;
  llvm::StringRef Name(*Import);
  if (!Name.consume_front("_"))
    return std::nullopt;

  SourceCallTypeHint Result;
  Result.CallKind = SourceCallTypeHint::Kind::SwiftRuntimeCall;
  Result.TargetAddress = ImportSlot;
  Result.TargetName = Name.str();
  if (declaredMetadataABI(Image, ImportSlot, Result))
    return Result;
  auto &Signature = Result.Signature;
  if (declaredSDKABI(Image, ImportSlot, Result))
    return Result;
  Signature.Origin = SourceFunctionTypeHint::OriginKind::SwiftRuntime;
  if (declaredStdlibABI(Image, ImportSlot, Name, Result))
    return Result;
  const auto Pointer = NdType::makePtr(NdType::makeVoid());
  const bool ReportInFile =
      Name == "_swift_stdlib_reportFatalErrorInFile" ||
      Name == "_swift_stdlib_reportUnimplementedInitializerInFile";
  const bool ReportInitializer =
      Name == "_swift_stdlib_reportUnimplementedInitializer" ||
      Name == "_swift_stdlib_reportUnimplementedInitializerInFile";
  if (ReportInFile || ReportInitializer ||
      Name == "_swift_stdlib_reportFatalError") {
    // SwiftShims/AssertionReporting.h declares ordinary C calls. Reporting
    // returns; the compiler emits a separate trap. Each string is consumed
    // through a bounded precision and copied into the diagnostic message.
    const auto Bytes = NdType::makePtr(NdType::makeInt(1, false));
    const auto Length = NdType::makeInt(4, true);
    const auto Unsigned = NdType::makeInt(4, false);
    Signature.ReturnType = NdType::makeVoid();
    Signature.Parameters = {{"first", Bytes},
                            {"first_length", Length},
                            {"second", Bytes},
                            {"second_length", Length}};
    Result.BorrowedByteInputs = {{0, 1}, {2, 3}};
    if (ReportInFile) {
      Signature.Parameters.push_back({"file", Bytes});
      Signature.Parameters.push_back({"file_length", Length});
      Signature.Parameters.push_back({"line", Unsigned});
      Result.BorrowedByteInputs.push_back({4, 5});
      if (ReportInitializer)
        Signature.Parameters.push_back({"column", Unsigned});
    }
    Signature.Parameters.push_back({"flags", Unsigned});
    std::string Diagnostic;
    if (!assignDarwinScalarSourceABI(Signature, Image.Arch, Diagnostic))
      return std::nullopt;
    return Result;
  }
  // These entries have C_CC declarations in the Swift runtime ABI. In
  // particular, Direct refcount entries use SwiftDirectRR_CC and must not be
  // accepted by prefix matching. Keep calls and their memory effects intact.
  // https://github.com/swiftlang/swift/blob/main/include/swift/Runtime/RuntimeFunctions.def
  if (Name == "swift_retain" || Name == "swift_nonatomic_retain" ||
      Name == "swift_unknownObjectRetain" ||
      Name == "swift_nonatomic_unknownObjectRetain" ||
      Name == "swift_bridgeObjectRetain" ||
      Name == "swift_nonatomic_bridgeObjectRetain" ||
      Name == "swift_getObjectType") {
    Signature.ReturnType = Pointer;
    Signature.Parameters = {{"object", Pointer}};
  } else if (Name == "swift_release" || Name == "swift_nonatomic_release" ||
             Name == "swift_unknownObjectRelease" ||
             Name == "swift_nonatomic_unknownObjectRelease" ||
             Name == "swift_bridgeObjectRelease" ||
             Name == "swift_nonatomic_bridgeObjectRelease") {
    Signature.ReturnType = NdType::makeVoid();
    Signature.Parameters = {{"object", Pointer}};
    // RuntimeFunctions.def instantiates this exact C-ABI storage family for
    // native and unknown-object weak/unowned references. Keep explicit names so
    // private suffix variants cannot acquire a public runtime contract.
  } else if (Name == "swift_weakInit" || Name == "swift_weakAssign" ||
             Name == "swift_unknownObjectWeakInit" ||
             Name == "swift_unknownObjectWeakAssign" ||
             Name == "swift_unownedInit" || Name == "swift_unownedAssign" ||
             Name == "swift_unknownObjectUnownedInit" ||
             Name == "swift_unknownObjectUnownedAssign") {
    Signature.ReturnType = Pointer;
    Signature.Parameters = {{"reference", Pointer}, {"object", Pointer}};
  } else if (Name == "swift_weakCopyInit" || Name == "swift_weakTakeInit" ||
             Name == "swift_weakCopyAssign" || Name == "swift_weakTakeAssign" ||
             Name == "swift_unknownObjectWeakCopyInit" ||
             Name == "swift_unknownObjectWeakTakeInit" ||
             Name == "swift_unknownObjectWeakCopyAssign" ||
             Name == "swift_unknownObjectWeakTakeAssign" ||
             Name == "swift_unownedCopyInit" ||
             Name == "swift_unownedTakeInit" ||
             Name == "swift_unownedCopyAssign" ||
             Name == "swift_unownedTakeAssign" ||
             Name == "swift_unknownObjectUnownedCopyInit" ||
             Name == "swift_unknownObjectUnownedTakeInit" ||
             Name == "swift_unknownObjectUnownedCopyAssign" ||
             Name == "swift_unknownObjectUnownedTakeAssign") {
    Signature.ReturnType = Pointer;
    Signature.Parameters = {{"destination", Pointer}, {"source", Pointer}};
  } else if (Name == "swift_weakLoadStrong" || Name == "swift_weakTakeStrong" ||
             Name == "swift_unknownObjectWeakLoadStrong" ||
             Name == "swift_unknownObjectWeakTakeStrong" ||
             Name == "swift_unownedLoadStrong" ||
             Name == "swift_unownedTakeStrong" ||
             Name == "swift_unknownObjectUnownedLoadStrong" ||
             Name == "swift_unknownObjectUnownedTakeStrong") {
    Signature.ReturnType = Pointer;
    Signature.Parameters = {{"reference", Pointer}};
  } else if (Name == "swift_weakDestroy" ||
             Name == "swift_unknownObjectWeakDestroy" ||
             Name == "swift_unownedDestroy" ||
             Name == "swift_unknownObjectUnownedDestroy") {
    Signature.ReturnType = NdType::makeVoid();
    Signature.Parameters = {{"reference", Pointer}};
  } else if (Name == "swift_once") {
    // Runtime/Once.h uses C_CC, including the context argument passed to the
    // callback. A source binding preserves the runtime call and its predicate;
    // it does not prove ownership or permit eager/omitted initialization.
    Signature.ReturnType = NdType::makeVoid();
    Signature.Parameters = {{"predicate", Pointer},
                            {"function", NdType::makePtr(NdType::makeFunc(
                                             NdType::makeVoid(), {Pointer}))},
                            {"context", Pointer}};
  } else if (Name == "swift_beginAccess") {
    Signature.ReturnType = NdType::makeVoid();
    Signature.Parameters = {{"address", Pointer},
                            {"scratch", Pointer},
                            {"flags", NdType::makeInt(8, false)},
                            {"pc", Pointer}};
  } else if (Name == "swift_endAccess") {
    Signature.ReturnType = NdType::makeVoid();
    Signature.Parameters = {{"scratch", Pointer}};
  } else if (Name == "swift_defaultActor_initialize" ||
             Name == "swift_defaultActor_destroy") {
    // Swift 6.1.2 arm64 client IR calls both actor lifecycle entries with
    // swiftcc void(ptr). The actor's storage is passed, not returned.
    const auto Bind = Image.DyldBindSlots.find(ImportSlot);
    if (Image.Arch != Arch::AArch64 || Bind == Image.DyldBindSlots.end() ||
        Bind->second.Module != "/usr/lib/swift/libswift_Concurrency.dylib")
      return std::nullopt;
    Signature.Convention = SourceFunctionTypeHint::ConventionKind::Swift;
    Signature.ReturnType = NdType::makeVoid();
    Signature.Parameters = {{"actor", Pointer}};
  } else if (Name == "swift_task_alloc" || Name == "swift_task_dealloc") {
    // Swift 6.1.2 arm64 async client IR declares swiftcc ptr(i64) and
    // swiftcc void(ptr), respectively. Preserve allocation and release as
    // real calls with the exact libswift_Concurrency provider.
    const auto Bind = Image.DyldBindSlots.find(ImportSlot);
    if (Image.Arch != Arch::AArch64 || Bind == Image.DyldBindSlots.end() ||
        Bind->second.Module != "/usr/lib/swift/libswift_Concurrency.dylib")
      return std::nullopt;
    Signature.Convention = SourceFunctionTypeHint::ConventionKind::Swift;
    if (Name == "swift_task_alloc") {
      Signature.ReturnType = Pointer;
      Signature.Parameters = {{"size", NdType::makeInt(8, false)}};
    } else {
      Signature.ReturnType = NdType::makeVoid();
      Signature.Parameters = {{"pointer", Pointer}};
    }
  } else if (!declaredFixedABI(Image, ImportSlot, Name, Result)) {
    return std::nullopt;
  }
  std::string Diagnostic;
  const bool Assigned =
      Signature.Convention == SourceFunctionTypeHint::ConventionKind::Swift
          ? assignDarwinSwiftSourceABI(Signature, Image.Arch, Diagnostic)
          : assignDarwinScalarSourceABI(Signature, Image.Arch, Diagnostic);
  if (!Assigned)
    return std::nullopt;
  return Result;
}

} // namespace neverd
