//===- CDialect.cpp - Emitted C printed back from its tree ----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Prints a read text back as C from its tree: statements and expressions
/// from their nodes, with the parentheses C's precedence needs added to the
/// ones the text wrote, and types as written.  Reading and printing back
/// must give the same tokens; a parse that grouped operators differently
/// than C does would print different ones.
///
//===----------------------------------------------------------------------===//

#include "DialectPrinter.h"

#include "llvm/ADT/StringSwitch.h"

using namespace neverd;
using namespace neverd::csyntax;

namespace {

/// C's binary precedence (C17 6.5): bitwise operators bind less tightly
/// than comparisons, unlike in Rust.
enum CPrecedence : int {
  CComma = 5,
  CAssign = 10,
  CConditional = 15,
  COr = 20,
  CAnd = 30,
  CBitOr = 40,
  CBitXor = 45,
  CBitAnd = 50,
  CEquality = 55,
  CRelational = 60,
  CShift = 65,
  CAdd = 70,
  CMul = 80,
};

int cPrecedence(llvm::StringRef Op) {
  return llvm::StringSwitch<int>(Op)
      .Cases({"*", "/", "%"}, CMul)
      .Cases({"+", "-"}, CAdd)
      .Cases({"<<", ">>"}, CShift)
      .Cases({"<", ">", "<=", ">="}, CRelational)
      .Cases({"==", "!="}, CEquality)
      .Case("&", CBitAnd)
      .Case("^", CBitXor)
      .Case("|", CBitOr)
      .Case("&&", CAnd)
      .Case("||", COr)
      .Default(CComma);
}

class CPrinter final : public DialectPrinter {
public:
  using DialectPrinter::DialectPrinter;

private:
  void header() override {}
  void directive(const TopLevel &Item) override { line(Item.Text); }
  std::string escapeIdentifier(llvm::StringRef Name) const override {
    return Name.str();
  }
  std::optional<std::string> sourceName(llvm::StringRef) const override {
    return std::nullopt;
  }

  llvm::StringRef text(size_t Begin, size_t End) const {
    return T.Source.slice(Begin, End).trim();
  }

  static std::string paren(const Printed &P, int Min) {
    return P.Prec >= Min ? P.Text : "(" + P.Text + ")";
  }

  Printed expr(const Expr *E) {
    Printed P = exprInner(E);
    return withComments(E, std::move(P));
  }

  Printed exprInner(const Expr *E) {
    switch (E->Kind) {
    case ExprKind::Name:
    case ExprKind::Integer:
    case ExprKind::Floating:
    case ExprKind::Character:
    case ExprKind::Asm:
      return Printed{E->Text.str(), PrecAtom};
    case ExprKind::String: {
      if (E->Ops.empty())
        return Printed{E->Text.str(), PrecAtom};
      std::string Text;
      for (const Expr *Part : E->Ops)
        Text += (Text.empty() ? "" : " ") + Part->Text.str();
      return Printed{Text, PrecAtom};
    }
    case ExprKind::Paren:
      return Printed{"(" + expr(E->Ops.front()).Text + ")", PrecAtom};
    case ExprKind::Unary: {
      Printed Op = expr(E->Ops.front());
      if (E->Postfix)
        return Printed{paren(Op, PrecAtom) + E->Text.str(), PrecAtom};
      // `- -x` must not print as `--x`.
      std::string Sep = (E->Text == "-" || E->Text == "+") &&
                                !Op.Text.empty() && Op.Text[0] == E->Text[0]
                            ? " "
                            : "";
      return Printed{E->Text.str() + Sep + paren(Op, PrecUnary), PrecUnary};
    }
    case ExprKind::Binary: {
      const int Prec = cPrecedence(E->Text);
      return Printed{paren(expr(E->Ops[0]), Prec) + " " + E->Text.str() + " " +
                         paren(expr(E->Ops[1]), Prec + 1),
                     Prec};
    }
    case ExprKind::Assign:
      return Printed{paren(expr(E->Ops[0]), PrecUnary) + " " + E->Text.str() +
                         " " + paren(expr(E->Ops[1]), CAssign),
                     CAssign};
    case ExprKind::Conditional:
      return Printed{paren(expr(E->Ops[0]), COr) + " ? " +
                         expr(E->Ops[1]).Text + " : " +
                         paren(expr(E->Ops[2]), CConditional),
                     CConditional};
    case ExprKind::Comma: {
      std::string Text;
      for (const Expr *Op : E->Ops)
        Text += (Text.empty() ? "" : ", ") + paren(expr(Op), CAssign);
      return Printed{Text, CComma};
    }
    case ExprKind::Cast:
      return Printed{text(E->Begin, E->Ops.front()->Begin).str() +
                         paren(expr(E->Ops.front()), PrecUnary),
                     PrecUnary};
    case ExprKind::Call: {
      std::string Text = paren(expr(E->Ops.front()), PrecAtom) + "(";
      for (size_t I = 1; I < E->Ops.size(); ++I)
        Text += (I > 1 ? ", " : "") + paren(expr(E->Ops[I]), CAssign);
      return Printed{Text + ")", PrecAtom};
    }
    case ExprKind::Subscript:
      return Printed{paren(expr(E->Ops[0]), PrecAtom) + "[" +
                         expr(E->Ops[1]).Text + "]",
                     PrecAtom};
    case ExprKind::Member:
      return Printed{paren(expr(E->Ops.front()), PrecAtom) +
                         (E->Postfix ? "->" : ".") + E->Text.str(),
                     PrecAtom};
    case ExprKind::SizeofExpr:
      return Printed{"sizeof " + paren(expr(E->Ops.front()), PrecUnary),
                     PrecUnary};
    case ExprKind::SizeofType:
    case ExprKind::AlignofType:
    case ExprKind::Offsetof:
      return Printed{text(E->Begin, E->End).str(), PrecAtom};
    case ExprKind::CompoundLiteral:
      return Printed{text(E->Begin, E->Ops.front()->Begin).str() + " " +
                         expr(E->Ops.front()).Text,
                     PrecAtom};
    case ExprKind::InitList: {
      std::string Text = "{ ";
      for (size_t I = 0; I < E->Ops.size(); ++I)
        Text += (I ? ", " : "") + expr(E->Ops[I]).Text;
      return Printed{Text + (E->Postfix ? ", }" : " }"), PrecAtom};
    }
    case ExprKind::Designated:
      if (E->Text.empty())
        return Printed{"[" + expr(E->Ops[0]).Text +
                           "] = " + expr(E->Ops[1]).Text,
                       PrecAtom};
      return Printed{"." + E->Text.str() + " = " + expr(E->Ops[0]).Text,
                     PrecAtom};
    case ExprKind::StatementExpr: {
      std::string Body = capture([&] { stmt(E->Body); });
      return Printed{"(" + llvm::StringRef(Body).trim().str() + ")", PrecAtom};
    }
    case ExprKind::BitCast:
      return Printed{text(E->Begin, E->Ops.front()->Begin).str() +
                         expr(E->Ops.front()).Text + ")",
                     PrecAtom};
    }
    return Printed{"", PrecAtom};
  }

  void body(const Stmt *S) {
    if (S->Kind != StmtKind::Compound) {
      stmt(S);
      return;
    }
    leadingComments(S->Leading);
    for (const Stmt *Child : S->Body)
      stmt(Child);
  }

  void block(llvm::StringRef Head, const Stmt *S) {
    line((Head + (Head.empty() ? "{" : " {")).str());
    indent();
    body(S);
    dedent();
  }

  void stmt(const Stmt *S) {
    leadingComments(S->Leading);
    switch (S->Kind) {
    case StmtKind::Compound:
      block("", S);
      line("}");
      break;
    case StmtKind::Declaration:
      for (const Decl *D : S->Decls)
        declarationLine(D);
      break;
    case StmtKind::Expression:
      line(expr(S->Value).Text + ";");
      break;
    case StmtKind::If:
      if (S->Then->Kind != StmtKind::Compound) {
        // An unbraced body stays unbraced.
        line("if (" + expr(S->Value).Text + ")");
        indent();
        stmt(S->Then);
        dedent();
        if (S->Else) {
          line("else");
          indent();
          stmt(S->Else);
          dedent();
        }
        break;
      }
      block("if (" + expr(S->Value).Text + ")", S->Then);
      if (S->Else) {
        line("} else {");
        indent();
        body(S->Else);
        dedent();
      }
      line("}");
      break;
    case StmtKind::While:
      block("while (" + expr(S->Value).Text + ")", S->Then);
      line("}");
      break;
    case StmtKind::DoWhile:
      block("do", S->Then);
      line("} while (" + expr(S->Value).Text + ");");
      break;
    case StmtKind::For:
      block("for (" + (S->Value ? expr(S->Value).Text : "") + "; " +
                (S->Else ? expr(S->Else->Value).Text : "") + "; " +
                (S->Step ? expr(S->Step).Text : "") + ")",
            S->Then);
      line("}");
      break;
    case StmtKind::Switch:
      block("switch (" + expr(S->Value).Text + ")", S->Then);
      line("}");
      break;
    case StmtKind::Case:
      line("case " + expr(S->Value).Text + ":");
      break;
    case StmtKind::Default:
      line("default:");
      break;
    case StmtKind::Break:
      line("break;");
      break;
    case StmtKind::Continue:
      line("continue;");
      break;
    case StmtKind::Return:
      line(S->Value ? "return " + expr(S->Value).Text + ";" : "return;");
      break;
    case StmtKind::Goto:
      line("goto " + S->Text.str() + ";");
      break;
    case StmtKind::Label:
      line(S->Text.str() + ":");
      break;
    case StmtKind::Empty:
      line(";");
      break;
    case StmtKind::Comment:
      line(commentText(S->TheComment));
      break;
    case StmtKind::Try:
      block(S->CxxTry ? "try" : "__try", S->Then);
      for (const Handler &H : S->Handlers) {
        leadingComments(H.Leading);
        switch (H.TheKind) {
        case Handler::Kind::Except:
          line("} __except (" + expr(H.Filter).Text + ") {");
          break;
        case Handler::Kind::Finally:
          line("} __finally {");
          break;
        case Handler::Kind::Catch:
          line("} catch (" + H.Parameter.str() + ") {");
          break;
        }
        indent();
        body(H.Body);
        dedent();
      }
      line("}");
      break;
    case StmtKind::Throw:
      line(S->Value ? "throw " + expr(S->Value).Text + ";" : "throw;");
      break;
    case StmtKind::PseudoBlock:
      block(S->Text, S->Then);
      line("}");
      break;
    case StmtKind::Leave:
      line("__leave;");
      break;
    case StmtKind::Asm:
      line(T.Source.slice(S->Begin, S->End).str() +
           (T.Source.slice(S->Begin, S->End).ends_with(";") ? "" : ";"));
      break;
    }
    trailingComments(S->Trailing);
  }

  void declarationLine(const Decl *D) {
    if (D->Body) {
      line(text(D->Begin, D->Body->Begin).str() + " {");
      indent();
      body(D->Body);
      dedent();
      line("}");
      return;
    }
    if (D->Init) {
      line(text(D->Begin, D->Init->Begin).str() + " " + expr(D->Init).Text +
           ";");
      return;
    }
    line(text(D->Begin, D->End).str() + ";");
  }

  void declaration(const TopLevel &Item) override {
    if (Item.Decls.size() > 1 || Item.Decls.front()->Kind == DeclKind::Record ||
        Item.Decls.front()->Kind == DeclKind::Typedef) {
      line(text(Item.Begin, Item.End).str());
    } else {
      declarationLine(Item.Decls.front());
    }
    trailingComments(Item.Trailing);
  }

  void staticAssert(const TopLevel &Item) override {
    line("_Static_assert(" + expr(Item.Assertion).Text + ", " +
         expr(Item.Message).Text + ");");
    trailingComments(Item.Trailing);
  }
};

} // namespace

std::unique_ptr<DialectPrinter>
csyntax::makeCPrinter(const Tree &T, const SourceDialectOptions &Opts) {
  return std::make_unique<CPrinter>(T, Opts);
}
