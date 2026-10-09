//===- SymbolSpellingTests.cpp - How symbol names read --------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/loader/SymbolSpelling.h"

using namespace neverd;

namespace {

TEST(SymbolSpelling, CxxNamesReadAsTheirDemanglersPrintThem) {
  EXPECT_EQ(symbolScheme("_ZNK8QDomNode8nodeTypeEv"), SymbolScheme::Itanium);
  EXPECT_EQ(symbolSchemeLanguage(SymbolScheme::Itanium), "c++");
  EXPECT_EQ(readableSymbolName("_ZNK8QDomNode8nodeTypeEv"),
            "QDomNode::nodeType() const");
  // Mach-O adds one underscore to every symbol.
  EXPECT_EQ(readableSymbolName("__ZN3BarC1Ev"), "Bar::Bar()");
  // The access specifier says nothing a reader of the code can use.
  EXPECT_EQ(symbolScheme("?nodeType@QDomNode@@QEBAHXZ"),
            SymbolScheme::Microsoft);
  EXPECT_EQ(readableSymbolName("?nodeType@QDomNode@@QEBAHXZ"),
            "int __cdecl QDomNode::nodeType(void) const");
  // C++ identifiers follow the C backend's own rules.
  EXPECT_TRUE(symbolIdentifierStem("_ZNK8QDomNode8nodeTypeEv").empty());
  // A name that claims Itanium and does not demangle keeps its spelling.
  EXPECT_TRUE(readableSymbolName("_Zebra").empty());
}

TEST(SymbolSpelling, CxxNamesReadWithoutDefaultTemplateArguments) {
  // libstdc++'s C++11 ABI namespace, the string's default arguments and its
  // standard name, and the destructor named after it.
  EXPECT_EQ(readableSymbolName(
                "_ZNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEED1Ev"),
            "std::string::~string()");
  EXPECT_EQ(readableSymbolName("_ZNSt6vectorIiSaIiEE9push_backERKi"),
            "std::vector<int>::push_back(int const&)");
  // Arguments inside arguments, and parameter types.
  EXPECT_EQ(readableSymbolName("_ZNSt3mapIiSt6vectorIiSaIiEESt4lessIiESaISt4pa"
                               "irIKiS2_EEEixERS6_"),
            "std::map<int, std::vector<int>>::operator[](int const&)");
  EXPECT_EQ(readableSymbolName("_ZSt4endlIcSt11char_traitsIcEERSt13basic_ostre"
                               "amIT_T0_ES6_"),
            "std::ostream& std::endl<char, std::char_traits<char>>("
            "std::ostream&)");
  // The operator comes before a function template's arguments, which a
  // space keeps apart from it.
  EXPECT_EQ(readableSymbolName("_ZStlsISt11char_traitsIcEERSt13basic_ostreamIc"
                               "T_ES5_PKc"),
            "std::ostream& std::operator<< <std::char_traits<char>>("
            "std::ostream&, char const*)");
  // An argument that differs from its default stays.
  EXPECT_EQ(readableSymbolName("_ZNSt6vectorIi7MyAllocIiEE5clearEv"),
            "std::vector<int, MyAlloc<int>>::clear()");
  // Microsoft's class keywords in template arguments go too.
  EXPECT_EQ(readableSymbolName("?push_back@?$vector@HV?$allocator@H@std@@@std@@"
                               "QEAAXAEBH@Z"),
            "void __cdecl std::vector<int>::push_back(int const &)");
}

TEST(SymbolSpelling, LegacyRustReadsWithoutItsHash) {
  constexpr const char *Write = "_ZN4core3fmt5write17h0123456789abcdefE";
  EXPECT_EQ(symbolScheme(Write), SymbolScheme::RustLegacy);
  EXPECT_EQ(symbolSchemeLanguage(SymbolScheme::RustLegacy), "rust");
  EXPECT_EQ(readableSymbolName(Write), "core::fmt::write");
  EXPECT_EQ(symbolIdentifierStem(Write), "core_fmt_write");
  EXPECT_EQ(readableSymbolName("__ZN4core3fmt5write17h0123456789abcdefE"),
            "core::fmt::write");

  // Escaped path characters, a trait implementation and a closure.
  constexpr const char *WriteStr =
      "_ZN58_$LT$alloc..string..String$u20$as$u20$core..fmt..Write$GT$"
      "9write_str17h3333333333333333E";
  EXPECT_EQ(readableSymbolName(WriteStr),
            "<alloc::string::String as core::fmt::Write>::write_str");
  EXPECT_EQ(symbolIdentifierStem(WriteStr), "String_Write_write_str");
  constexpr const char *Closure =
      "_ZN4main4main28_$u7b$$u7b$closure$u7d$$u7d$17h2222222222222222E";
  EXPECT_EQ(readableSymbolName(Closure), "main::main::{{closure}}");
  EXPECT_EQ(symbolIdentifierStem(Closure), "main_main_closure");
  // `$u4e2d$` is U+4E2D; the C backend escapes the stem's UTF-8 bytes.
  EXPECT_EQ(readableSymbolName("_ZN4main7$u4e2d$17h0123456789abcdefE"),
            "main::中");

  // Without the hash it is an ordinary Itanium name.
  EXPECT_EQ(symbolScheme("_ZN3foo3barE"), SymbolScheme::Itanium);
  EXPECT_EQ(readableSymbolName("_ZN3foo3barE"), "foo::bar");
}

TEST(SymbolSpelling, V0RustPathsName) {
  constexpr const char *WriteFmt =
      "_RNvYNtNtCscdodAO9FK5_5alloc6string6StringNtNtCs4NRVxsYgnAr_4core3fmt"
      "5Write9write_fmtB6_";
  EXPECT_EQ(symbolScheme(WriteFmt), SymbolScheme::RustV0);
  EXPECT_EQ(readableSymbolName(WriteFmt),
            "<alloc::string::String as core::fmt::Write>::write_fmt");
  EXPECT_EQ(symbolIdentifierStem(WriteFmt), "String_Write_write_fmt");
  // An inherent method reads under the type's whole path.
  constexpr const char *WriteStr =
      "_RNvMsa_NtCs4NRVxsYgnAr_4core3fmtNtB5_9Formatter9write_str";
  EXPECT_EQ(readableSymbolName(WriteStr), "<core::fmt::Formatter>::write_str");
  EXPECT_EQ(symbolIdentifierStem(WriteStr), "core_fmt_Formatter_write_str");
  // A closure's vtable shim, behind a qualified self type that is itself a
  // closure.
  constexpr const char *Shim =
      "_RNSNvYNCINvMs0_NtNtCs2AWtUsOyxgP_3std4sync4onceNtBd_4Once9call_"
      "onceNCNvNtBh_2rt7cleanup0E0INtNtNtCs4NRVxsYgnAr_4core3ops8function6"
      "FnOnceTRNtBd_9OnceStateEE9call_once6vtableBh_";
  EXPECT_EQ(symbolIdentifierStem(Shim), "closure_FnOnce_call_once_vtable_shim");
  // Generic arguments do not name the item.
  constexpr const char *DropGlue =
      "_RINvNtCs4NRVxsYgnAr_4core3ptr9drop_glueINtNtCsdB6qrhj7hiN_9addr2line"
      "4unit8ResUnitsINtNtNtCsjd0ZH04R2Z3_5gimli4read12endian_slice11Endian"
      "SliceNtNtB1p_9endianity12LittleEndianEEECs2AWtUsOyxgP_3std";
  EXPECT_EQ(symbolIdentifierStem(DropGlue), "core_ptr_drop_glue");
}

TEST(SymbolSpelling, NamesStartingWithRAreNotRust) {
  // A v0 path starts with a tag; MSVC's run-time checks and Windows APIs
  // spell no such tag.
  for (const char *Name : {"_RTC_CheckEsp", "ReadFile", "Run", "_Rf"})
    EXPECT_EQ(symbolScheme(Name), SymbolScheme::None) << Name;
}

TEST(SymbolSpelling, SwiftNamesReadAsDeclarationPaths) {
  EXPECT_EQ(symbolScheme("$s4Demo6answers5Int32VyF"), SymbolScheme::Swift);
  EXPECT_EQ(readableSymbolName("$s4Demo6answers5Int32VyF"), "Demo.answer()");
  EXPECT_EQ(symbolIdentifierStem("$s4Demo6answers5Int32VyF"), "Demo_answer");
  EXPECT_EQ(readableSymbolName("$s4Demo3BoxC5countSivg"),
            "Demo.Box.count.getter");
  EXPECT_EQ(symbolIdentifierStem("$s4Demo3BoxC5countSivg"),
            "Demo_Box_count_getter");
  // Mach-O's underscore.
  EXPECT_EQ(readableSymbolName("_$s4Demo3BoxCACycfC"),
            "Demo.Box.__allocating_init()");
  EXPECT_EQ(symbolIdentifierStem("_$s4Demo3BoxCACycfC"),
            "Demo_Box_allocating_init");
  EXPECT_EQ(readableSymbolName("$s4Demo3BoxCfD"),
            "Demo.Box.__deallocating_deinit");
  // Labels name the arguments; unlabeled ones read `_:`.
  EXPECT_EQ(readableSymbolName("$s4main3foo1x1yS2i_SStF"), "main.foo(x:y:)");
  EXPECT_EQ(symbolIdentifierStem("$s4main3foo1x1yS2i_SStF"), "main_foo_x_y");
  EXPECT_EQ(readableSymbolName("$s4main3BarV6update4withySi_tF"),
            "main.Bar.update(with:)");
  EXPECT_EQ(readableSymbolName("$s4main3FooCyS2icig"),
            "main.Foo.subscript(_:).getter");
  // A private declaration reads by its name, without its file discriminator.
  EXPECT_EQ(readableSymbolName(
                "$s4main3FooC3bar33_0123456789ABCDEF0123456789ABCDEFLLyyF"),
            "main.Foo.bar()");
  // Thunks, accessors of a type and merged functions.
  EXPECT_EQ(readableSymbolName("$s4Demo1PP4nameSSvgTj"),
            "dispatch thunk of Demo.P.name.getter");
  EXPECT_EQ(symbolIdentifierStem("$s4Demo1PP4nameSSvgTj"),
            "Demo_P_name_getter_dispatch_thunk");
  EXPECT_EQ(readableSymbolName("$s4main3FooCMa"),
            "type metadata accessor for main.Foo");
  EXPECT_EQ(readableSymbolName("$s4main3fooyyFTm"), "merged main.foo()");
}

TEST(SymbolSpelling, DNamesReadAsQualifiedNames) {
  EXPECT_EQ(symbolScheme("_D10d_eh_probe12cleanupCounti"), SymbolScheme::D);
  EXPECT_EQ(readableSymbolName("_D10d_eh_probe12cleanupCounti"),
            "d_eh_probe.cleanupCount");
  EXPECT_EQ(symbolIdentifierStem("_D10d_eh_probe12cleanupCounti"),
            "d_eh_probe_cleanupCount");
}

TEST(SymbolSpelling, ObjCMethodsTakeTheGnuRuntimeName) {
  EXPECT_EQ(symbolScheme("-[NSString length]"), SymbolScheme::ObjCMethod);
  // The name already reads as Objective-C spells it.
  EXPECT_EQ(readableSymbolName("-[NSString length]"), "-[NSString length]");
  EXPECT_EQ(symbolIdentifierStem("-[NSString length]"), "_i_NSString__length");
  EXPECT_EQ(symbolIdentifierStem("+[NSObject(Extras) make:with:]"),
            "_c_NSObject_Extras_make_with_");
  for (const char *Name :
       {"-[NoSelector]", "-[A(B C]", "+[ sel]", "-[A b;c]", "-[A(B;) c]"})
    EXPECT_EQ(symbolScheme(Name), SymbolScheme::None) << Name;
}

TEST(SymbolSpelling, UnmangledNamesReadAsTheyAre) {
  for (const char *Name : {"printf", "main.main", "fmt.(*pp).doPrintf",
                           "foo.constprop.0", "__libc_start_main"}) {
    EXPECT_EQ(symbolScheme(Name), SymbolScheme::None) << Name;
    EXPECT_TRUE(readableSymbolName(Name).empty()) << Name;
    EXPECT_TRUE(symbolIdentifierStem(Name).empty()) << Name;
  }
}

} // namespace
