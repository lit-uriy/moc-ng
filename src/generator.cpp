/****************************************************************************
 * Copyright (C) 2012 Woboq UG (haftungsbeschraenkt)
 * Olivier Goffart <contact at woboq.com>
 * http://woboq.com/
 ****************************************************************************/


#include "generator.h"
#include "mocng.h"
#include <string>
#include <type_traits>
#include <clang/AST/DeclCXX.h>
#include <clang/AST/ASTContext.h>


//  foreach method,  including  clones
template<typename T, typename F>
static void ForEachMethod(const std::vector<T> &V, F && Functor) {
    for(auto it : V) {
        int Clones = it->getNumParams() - it->getMinRequiredArguments();
        for (int C = 0; C <= Clones; ++C)
            Functor(it, C);
    }
}

template<typename T> int CountMethod(const T& V) {
    int R  = 0;
    ForEachMethod(V, [&](const clang::CXXMethodDecl*, int) { R++; });
    return R;
}

template<typename T> int AggregatePerameterCount(const std::vector<T>& V) {
    int R = 0;
    ForEachMethod(V, [&](const clang::CXXMethodDecl *M, int C) {
        R += M->getNumParams() - C;
        R += 1; // return value;
    });
    return R;
}
/*
template<typename F>
int ForAll(const ClassDef *CDef, F && Functor , bool Constructors = false) {
    int R = Functor(CDef->Signals) + Functor(CDef->Slots) + Functor(CDef->Methods);
    if (Constructors)
        R += Functor(CDef->Constructors);
    return R;
}*/

template <typename T>
void Generator::GenerateFunction(const T& V, const char* TypeName, MethodFlags Type, int& ParamIndex)
{
    if (V.empty())
        return;

    OS << "\n // " << TypeName << ": name, argc, parameters, tag, flags\n";

    ForEachMethod(V, [&](const clang::CXXMethodDecl *M, int C) {
        unsigned int Flags = Type;
        if (Type == MethodSignal)
            Flags |= AccessProtected;  // That's what moc beleive
        else if (M->getAccess() == clang::AS_private)
            Flags |= AccessPrivate;
        else if (M->getAccess() == clang::AS_public)
            Flags |= AccessPublic;
        else if (M->getAccess() == clang::AS_protected)
            Flags |= AccessProtected;

        if (C)
            Flags |= MethodCloned;

        for (auto attr_it = M->specific_attr_begin<clang::AnnotateAttr>();
             attr_it != M->specific_attr_end<clang::AnnotateAttr>();
             ++attr_it) {
            const clang::AnnotateAttr *A = *attr_it;
            if (A->getAnnotation() == "qt_scriptable") {
                Flags |= MethodScriptable;
            } else if (A->getAnnotation().startswith("qt_revision:")) {
                Flags |= MethodRevisioned;
            } else if (A->getAnnotation() == "qt_moc_compat") {
                Flags |= MethodCompatibility;
            }
        }

        int argc =  M->getNumParams() - C;

        OS << "    " << (M->getIdentifier() ? StrIdx(M->getName()) : StrIdx("")) << ", " << argc << ", " << ParamIndex << ", 0, 0x";
        OS.write_hex(Flags) << ",\n";
        ParamIndex += 1 + argc * 2;
    });
}

void Generator::GetTypeInfo(clang::QualType Type)
{
    if (Type->isVoidType()) {
        OS << "QMetaType::Void";
        return;
    }
    // FIXME:  Find the QMetaType
    OS << "0x80000000 | " << StrIdx(Type.getAsString(PrintPolicy));
}


template <typename T>
void Generator::GenerateFunctionParameters(const T& V, const char* TypeName)
{
    if (V.empty())
        return;

    OS << "\n // " << TypeName << ": parameters\n";

    ForEachMethod(V, [&](const clang::CXXMethodDecl *M, int C) {
        int argc =  M->getNumParams() - C;
        OS << "   "; // only 3 ' ';
        //Types
        OS << " ";
        GetTypeInfo(M->getResultType());
        OS <<  ",";
        for (int j = 0; j < argc; j++) {
            OS << " ";
            GetTypeInfo(M->getParamDecl(j)->getOriginalType());
            OS <<  ",";
        }

        //Names
        for (int j = 0; j < argc; j++) {
            auto P = M->getParamDecl(j);
            if (P->getIdentifier())
                OS << " " << StrIdx(P->getName()) << ",";
            else
                OS << " " << StrIdx("") << ",";
        }
        OS << "\n";
    });
}



Generator::Generator(const ClassDef* CDef, llvm::raw_ostream& OS, clang::ASTContext & Ctx) :
    CDef(CDef), OS(OS), Ctx(Ctx), PrintPolicy(Ctx.getPrintingPolicy())
{
    QualName = CDef->Record->getQualifiedNameAsString();

    PrintPolicy.SuppressTagKeyword = true;

    if (CDef->Record->getNumBases())
        BaseName = CDef->Record->bases_begin()->getType().getAsString(PrintPolicy);

    MethodCount = CountMethod(CDef->Signals) + CountMethod(CDef->Slots) + CountMethod(CDef->Methods);
}

void Generator::GenerateCode()
{
    enum { OutputRevision = 7,
          MetaObjectPrivateFieldCount = 14 //  = sizeof(QMetaObjectPrivate) / sizeof(int)
    };

    // Build the data array
    std::string QualifiedClassNameIdentifier = QualName;
    std::replace(QualifiedClassNameIdentifier.begin(), QualifiedClassNameIdentifier.end(), ':', '_');

    int Index = MetaObjectPrivateFieldCount;

    //Helper function which adds N to the index and return a value suitable to be placed in the array.
    auto I = [&](int N) {
        if (!N) return 0;
        int R = Index;
        Index += N;
        return R;
    };

    OS << "\nstatic const uint qt_meta_data_" << QualifiedClassNameIdentifier << "[] = {\n"
          "    " << OutputRevision << ", // revision\n"
          "    " << StrIdx(QualName) << ", // classname\n"
          "    " << CDef->ClassInfo.size() << ", " << I(CDef->ClassInfo.size()) << ", //classinfo\n";

    OS << "    " << MethodCount << ", " << I(MethodCount * 5) << ", // methods \n";

     // TODO: REVISON   Index += NumberRevisionedMethod

    int ParamsIndex = Index;
    int TotalParameterCount = AggregatePerameterCount(CDef->Signals) + AggregatePerameterCount(CDef->Slots)
                            + AggregatePerameterCount(CDef->Methods) + AggregatePerameterCount(CDef->Constructors);
    Index += TotalParameterCount * 2 // type and parameter names
           - MethodCount - CountMethod(CDef->Constructors);  // return parameter don't have names

    OS << "    " << CDef->Properties.size() << ", " << I(CDef->Properties.size() * 3) << ", // properties \n";

    // TODO: REVISON + NOTIDY   Index += Number Notify +  Number Revision


    OS << "    " << CDef->Enums.size() << ", " << I(CDef->Enums.size() * 4) << ", // enums \n";

    // TODO: enum values

    int ConstructorCount = CountMethod(CDef->Constructors);
    OS << "    " << ConstructorCount << ", " << I(ConstructorCount * 5) << ", // constructors \n";

    OS << "    " << 0 << ", // flags \n";

    OS << "    " << CountMethod(CDef->Signals) << ", // signalCount \n";


    if (CDef->ClassInfo.size()) {
        OS << "\n  // classinfo: key, value\n";
        for (const auto &I : CDef->ClassInfo)
            OS << "    " << StrIdx(I.first) << ", " << StrIdx(I.second) << ",\n";
    }


    GenerateFunction(CDef->Signals, "signals", MethodSignal, ParamsIndex);
    GenerateFunction(CDef->Slots, "slots", MethodSlot, ParamsIndex);
    GenerateFunction(CDef->Methods, "methods", MethodMethod, ParamsIndex);


    // TODO: revisions


    GenerateFunctionParameters(CDef->Signals, "signals");
    GenerateFunctionParameters(CDef->Slots, "slots");
    GenerateFunctionParameters(CDef->Methods, "methods");
    GenerateFunctionParameters(CDef->Constructors, "constructors");

    GenerateProperties();


    //TODO: enums

    GenerateFunction(CDef->Constructors, "constructors", MethodMethod, ParamsIndex);

    OS << "\n    0    // eod\n};\n";



    // StringArray;

    int TotalLen = 1;
    for (const auto &S : Strings)
        TotalLen += S.size() + 1;


    OS << "struct qt_meta_stringdata_" << QualifiedClassNameIdentifier << "_t {\n"
          "    QByteArrayData data[" << Strings.size() << "];\n"
          "    char stringdata[" << TotalLen << "];\n"
          "};\n"
          "#define QT_MOC_LITERAL(idx, ofs, len) \\\n"
          "    Q_STATIC_BYTE_ARRAY_DATA_HEADER_INITIALIZER_WITH_OFFSET(len, \\\n"
          "    offsetof(qt_meta_stringdata_"<<  QualifiedClassNameIdentifier << "_t, stringdata) + ofs \\\n"
          "        - idx * sizeof(QByteArrayData) \\\n"
          "    )\n"
          "static const qt_meta_stringdata_"<<  QualifiedClassNameIdentifier << "_t qt_meta_stringdata_"<<  QualifiedClassNameIdentifier << " = {\n"
          "    {\n";
    int Idx = 0;
    int LitteralIndex = 0;
    for (const auto &S : Strings) {
        if (LitteralIndex)
            OS << ",\n";
        OS << "QT_MOC_LITERAL("<< (LitteralIndex++) << ", " << Idx << ", " << S.size() << ")";
        Idx += S.size() + 1;
    }
    OS << "\n    },\n    \"";
    int Col = 0;
    for (const auto &S : Strings) {
        if (Col && Col + S.size() >= 72) {
            OS << "\"\n    \"";
            Col = 0;
        } else if (S.size() && S[0] >= '0' && S[0] <= '9') {
            OS << "\"\"";
        }
        OS.write_escaped(S) << "\\0";
        Col += 2 + S.size();
    }
    OS << "\"\n};\n"
          "#undef QT_MOC_LITERAL\n";

    // TODO: extra array


    OS << "const QMetaObject " << QualName << "::staticMetaObject = {\n"
          "    { ";
    if (BaseName.empty()) OS << "0";
    else OS << "&" << BaseName << "::staticMetaObject";

    OS << ", qt_meta_stringdata_"<< QualifiedClassNameIdentifier <<".data,\n"
          "      qt_meta_data_" << QualifiedClassNameIdentifier << ", ";

    if (CDef->HasQObject) OS << "qt_static_metacall";
    else OS << "0";

    OS << ", 0, 0}\n};\n";

    if (CDef->HasQObject) {
        OS << "const QMetaObject *" << QualName << "::metaObject() const\n{\n"
              "    return QObject::d_ptr->metaObject ? QObject::d_ptr->dynamicMetaObject() : &staticMetaObject;\n}\n";


        OS << "void *" << QualName << "::qt_metacast(const char *_clname)\n{\n"
              "    if (!_clname) return 0;\n"
              "    if (!strcmp(_clname, qt_meta_stringdata_" << QualifiedClassNameIdentifier << ".stringdata))\n"
              "        return static_cast<void*>(const_cast<" <<  QualName << "*>(this));\n"
              "    return "<< BaseName <<"::qt_metacast(_clname);\n"
              "}\n";

        GenerateMetaCall();
        GenerateStaticMetaCall();

        int SigIdx = 0;
        for (const clang::CXXMethodDecl *MD : CDef->Signals) {
            GenerateSignal(MD, SigIdx);
            SigIdx += 1 + MD->getNumParams() - MD->getMinRequiredArguments();
        }
    }

}

void Generator::GenerateMetaCall()
{
    OS << "\nint " << QualName << "::qt_metacall(QMetaObject::Call _c, int _id, void **_a)\n{\n"
          "    _id = " << BaseName << "::qt_metacall(_c, _id, _a);\n"
          "    if (_id < 0)\n"
          "        return _id;\n";
//          "    ";

    if (MethodCount) {
        OS << "    if (_c == QMetaObject::InvokeMetaMethod || _c == QMetaObject::RegisterMethodArgumentMetaType) {\n"
              "        if (_id < " << MethodCount << ")\n"
              "            qt_static_metacall(this, _c, _id, _a);\n"
              "        _id -= " << MethodCount << ";\n"
              "    }\n";
    }

    if (CDef->Properties.size()) {
        bool needGet = false;
        //bool needTempVarForGet = false;
        bool needSet = false;
        bool needReset = false;
        bool needDesignable = false;
        bool needScriptable = false;
        bool needStored = false;
        bool needEditable = false;
        bool needUser = false;
        for (const PropertyDef &p : CDef->Properties) {
            needGet |= !p.read.empty() || !p.member.empty();
            /*if (!p.read.empty())
                needTempVarForGet |= (p.gspec != PropertyDef::PointerSpec
                && p.gspec != PropertyDef::ReferenceSpec);*/
            needSet |= !p.write.empty() || (!p.member.empty() && !p.constant);
            needReset |= !p.reset.empty();

            auto IsFunction = [](const std::string &S) { return S.size() && S[S.size()-1] == ')'; };
            needDesignable |= IsFunction(p.designable);
            needScriptable |= IsFunction(p.scriptable);
            needStored |= IsFunction(p.stored);
            needEditable |= IsFunction(p.editable);
            needUser |= IsFunction(p.user);
        }

        OS << "#ifndef QT_NO_PROPERTIES\n    ";
        if (MethodCount)
            OS << "else ";

        auto HandleProperty = [&](bool Need, const char *Action, const std::function<void(const PropertyDef &)> &F) {
            OS << "if (_c == QMetaObject::" << Action << ") {\n";
            if (Need) {
                OS << "        switch (_id) {\n";
                int I = 0;
                for (const PropertyDef &p : CDef->Properties) {
                    OS << "        case " << (I++) <<": ";
                    F(p);
                    OS << "break;\n";
                }
                OS << "        }";
            }
            OS << "        _id -= " << CDef->Properties.size() << ";\n    }";
        };

        HandleProperty(needGet, "ReadProperty", [&](const PropertyDef &p) {
            if (p.read.empty() && p.member.empty())
                return;

            //FIXME: enums case
            OS << "*reinterpret_cast< " << p.type << "*>(_a[0]) = ";
            if (p.inPrivateClass.size())
                OS << p.inPrivateClass << "->" ;
            if (!p.read.empty())
                OS << p.read << "(); ";
            else
                OS << p.member << "; ";
        });
        OS << " else ";
        HandleProperty(needSet, "WriteProperty", [&](const PropertyDef &p) {
            if (p.constant)
                return;
            if (p.write.empty() && p.member.empty())
                return;

            if (p.inPrivateClass.size())
                OS << p.inPrivateClass << "->" ;
            if (!p.write.empty())
                OS << p.write << "(";
            else
                OS << p.member << " =  ";
            OS << "*reinterpret_cast< " << p.type << "*>(_a[0])";

            if (!p.write.empty())
                OS << "); ";
            else
                OS << "; "; //FIXME: Notify signal
        });
        OS << " else ";
        HandleProperty(needReset, "ResetProperty", [&](const PropertyDef &p) {
            if (p.reset.empty() || p.reset[p.reset.size()-1] != ')')
                return;

            if (p.inPrivateClass.size())
                OS << p.inPrivateClass << "->" ;
            OS << p.reset << "; ";
        });

        typedef std::string (PropertyDef::*Accessor);
        auto HandleQueryProperty = [&](bool Need, const char *Action, Accessor A) {
            HandleProperty(Need, Action, [&](const PropertyDef &p) {
                const std::string &S = (p.*A);
                if (S.empty() || S[S.size()-1] != ')')
                    return;
                OS << "*reinterpret_cast<bool*>(_a[0]) = " << S << "; ";
            });
        };

        OS << " else ";
        HandleQueryProperty(needDesignable, "QueryPropertyDesignable", &PropertyDef::designable);
        OS << " else ";
        HandleQueryProperty(needScriptable, "QueryPropertyScriptable", &PropertyDef::scriptable);
        OS << " else ";
        HandleQueryProperty(needScriptable, "QueryPropertyStored", &PropertyDef::stored);
        OS << " else ";
        HandleQueryProperty(needEditable, "QueryPropertyEditable", &PropertyDef::editable);
        OS << " else ";
        HandleQueryProperty(needUser, "QueryPropertyUser", &PropertyDef::user);
        OS << " else ";
        HandleQueryProperty(needUser, "QueryPropertyUser", &PropertyDef::user);

        OS << " else if (_c == QMetaObject::RegisterPropertyMetaType) {\n"
              "        if (_id < " << CDef->Properties.size() <<  ")\n"
              "            qt_static_metacall(this, _c, _id, _a);\n"
              "        _id -= " << CDef->Properties.size() <<  ";\n"
              "    }\n"
              "#endif // QT_NO_PROPERTIES\n";
    }
    OS << "    return _id;"
          "}\n";
}

void Generator::GenerateStaticMetaCall()
{
    OS << "\nvoid " << QualName << "::qt_static_metacall(QObject *_o, QMetaObject::Call _c, int _id, void **_a)\n{\n    ";

    if (!CDef->Constructors.empty()) {
        OS << "    if (_c == QMetaObject::CreateInstance) {\n"
              "        switch (_id) {\n";

        int CtorIndex = 0;
        ForEachMethod(CDef->Constructors, [&](const clang::CXXConstructorDecl *MD, int C) {
            OS << "        case " << (CtorIndex++) << ": { QObject *_r = new " << QualName << "(";

            for (int j = 0 ; j < MD->getNumParams() - C; ++j) {
                if (j) OS << ",";
                //FIXME:  QPrivateSignal
                      OS << "*reinterpret_cast< " << Ctx.getPointerType(MD->getParamDecl(j)->getType().getNonReferenceType().getUnqualifiedType()).getAsString(PrintPolicy) << " >(_a[" << (j+1) << "])";
            }
            OS << ");\n            if (_a[0]) *reinterpret_cast<QObject**>(_a[0]) = _r; } break;\n";
        });
        OS << "        }\n"
              "    }";

        if (MethodCount)  OS << " else ";
    }

    if (MethodCount) {
        OS << "if (_c == QMetaObject::InvokeMetaMethod) {\n"
//            "        Q_ASSERT(staticMetaObject.cast(_o));\n"
              "        " << QualName <<" *_t = static_cast<" << QualName << " *>(_o);\n"
              "        switch(_id) {\n" ;
        int MethodIndex = 0;
        auto GenM = [&](const clang::CXXMethodDecl *MD, int C) {
            if (!MD->getIdentifier())
                return;

            OS << "        case " << MethodIndex << ": ";
            // Original moc don't support reference as return type: see  Moc::parseFunction
            bool IsVoid = MD->getResultType()->isVoidType() || MD->getResultType()->isReferenceType();
            if (!IsVoid)
                OS << "{ " << MD->getResultType().getUnqualifiedType().getAsString(PrintPolicy) << " _r =  ";

            OS << "_t->" << MD->getName() << "(";

            for (int j = 0 ; j < MD->getNumParams() - C; ++j) {
                if (j) OS << ",";
                //FIXME:  QPrivateSignal
                OS << "*reinterpret_cast< " << Ctx.getPointerType(MD->getParamDecl(j)->getType().getNonReferenceType().getUnqualifiedType()).getAsString(PrintPolicy) << " >(_a[" << (j+1) << "])";
            }
            OS << ");";
            if (!IsVoid) {
                OS << "\n            if (_a[0]) *reinterpret_cast< "
                   << Ctx.getPointerType(MD->getResultType().getNonReferenceType().getUnqualifiedType()).getAsString(PrintPolicy)
                   << " >(_a[0]) = _r; }";
            }
            OS <<  " break;\n";
            MethodIndex++;
        };
        ForEachMethod(CDef->Signals, GenM);
        ForEachMethod(CDef->Slots, GenM);
        // TODO: private slots
        ForEachMethod(CDef->Methods, GenM);
        OS << "        }\n"
              "    }";
    }
    if (!CDef->Signals.empty()) {

        int MethodIndex = 0;
        OS << " else if (_c == QMetaObject::IndexOfMethod) {\n"
              "        int *result = reinterpret_cast<int *>(_a[0]);\n"
              "        void **func = reinterpret_cast<void **>(_a[1]);\n";

        for (const clang::CXXMethodDecl *MD: CDef->Signals) {
            int Idx = MethodIndex;
            MethodIndex += MD->getNumParams() - MD->getMinRequiredArguments() + 1;
            if (MD->isStatic() || !MD->getIdentifier())
                continue;
            OS << "        {\n"
                  "            typedef " << MD->getResultType().getAsString(PrintPolicy) << " (" << QualName << "::*_t)(";
            for (int j = 0 ; j < MD->getNumParams(); ++j) {
                if (j) OS << ",";
                OS << MD->getParamDecl(j)->getType().getAsString(PrintPolicy);
            }
            if (MD->isConst()) OS << ") const;\n";
            else OS << ");\n";

            OS << "            if (*reinterpret_cast<_t *>(func) == static_cast<_t>(&"<< QualName <<"::"<< MD->getName() <<")) {\n"
                  "                *result = " << Idx << ";\n"
                  "            }\n"
                  "        }\n";
        }
        OS << "    }";
    }

    //TODO RegisterPropertyMetaType

#if 0
        if (methodList.empty()) {
            fprintf(out, "    Q_UNUSED(_o);\n");
            if (CDef->constructorList.empty() && automaticPropertyMetaTypes.empty() && methodsWithAutomaticTypesHelper(methodList).empty()) {
                fprintf(out, "    Q_UNUSED(_id);\n");
                fprintf(out, "    Q_UNUSED(_c);\n");
            }
        }
        if (!isUsed_a)
            fprintf(out, "    Q_UNUSED(_a);\n");

        fprintf(out, "}\n\n");
#endif
    OS << "\n    Q_UNUSED(_o); Q_UNUSED(_id); Q_UNUSED(_c); Q_UNUSED(_a);";
    OS << "\n}\n";
}

void Generator::GenerateSignal(const clang::CXXMethodDecl *MD, int Idx)
{
    if (MD->isPure())
        return;

    OS << "\n// SIGNAL " << Idx << "\n"
       << MD->getResultType().getAsString(PrintPolicy) << " " << MD->getQualifiedNameAsString() + "(";
    for (int j = 0 ; j < MD->getNumParams(); ++j) {
        if (j) OS << ",";
        OS << MD->getParamDecl(j)->getType().getAsString(PrintPolicy) << " _t" << (j+1);
        //FIXME PrivateSignal
    }
    OS << ")";
    std::string This = "this";
    if (MD->isConst()) {
        OS << " const";
        This = "const_cast< " + QualName + " *>(this)";
    }
    OS << "\n{\n";
    bool IsVoid = MD->getResultType()->isVoidType();
    if (IsVoid && MD->getNumParams() == 0) {
        OS << "    QMetaObject::activate(" << This << ", &staticMetaObject, " << Idx << ", 0);\n";
    } else {
        std::string T = MD->getResultType().getNonReferenceType().getUnqualifiedType().getAsString(PrintPolicy);
        if (MD->getResultType()->isPointerType()) {
            OS << "    " << MD->getResultType().getAsString(PrintPolicy) << " _t0 = 0;\n";
        } else if (!IsVoid) {
            OS << "    " << T << " _t0 = " << T << "();\n";
        }
        OS << "    void *_a[] = { ";
        if (IsVoid) OS << "0";
        else OS << "&_t0";


        for (int j = 0 ; j < MD->getNumParams(); ++j) {
            if (MD->getParamDecl(j)->getType().isVolatileQualified())
                OS << ", const_cast<void*>(reinterpret_cast<const volatile void*>(&_t" << (j+1) << "))";
            else
                OS << ", const_cast<void*>(reinterpret_cast<const void*>(&_t" << (j+1) << "))";
            //FIXME PrivateSignal
        }

        OS << " };\n"
              "    QMetaObject::activate(" << This << ", &staticMetaObject, " << Idx << ", _a);\n";

        if (!IsVoid)
            OS << "    return _t0;\n";
    }
    OS <<"}\n";
}

void Generator::GenerateProperties()
{
    if (CDef->Properties.empty())
        return;
    for (const PropertyDef &p : CDef->Properties) {
        unsigned int flags = Invalid;
        //FIXME:
        //if (!isBuiltinType(p.type))
        //    Flags |= EnumOrFlag;
        if (!p.member.empty() && !p.constant)
            flags |= Writable;
        if (!p.read.empty() || !p.member.empty())
            flags |= Readable;
        if (!p.write.empty()) {
            flags |= Writable;
        //if (p.stdCppSet())
        //    flags |= StdCppSet;
        }
        if (!p.reset.empty())
            flags |= Resettable;
        if (p.designable.empty())
            flags |= ResolveDesignable;
        else if (p.designable != "false")
            flags |= Designable;
        if (p.scriptable.empty())
            flags |= ResolveScriptable;
        else if (p.scriptable != "false")
            flags |= Scriptable;
        if (p.stored.empty())
            flags |= ResolveStored;
        else if (p.stored != "false")
            flags |= Stored;
        if (p.editable.empty())
            flags |= ResolveEditable;
        else if (p.editable != "false")
            flags |= Editable;
        if (p.user.empty())
            flags |= ResolveUser;
        else if (p.user != "false")
            flags |= User;
        if (p.notifyId != -1)
            flags |= Notify;
        if (p.revision > 0)
            flags |= Revisioned;
        if (p.constant)
            flags |= Constant;
        if (p.final)
            flags |= Final;
        OS << "    " << StrIdx(p.name) << ", 0x80000000 | " << StrIdx(p.type) << ", 0x";
        OS.write_hex(flags) << ", // " << p.name << "\n";
    }
    //TODO: NOTIFY + REVISION
}



int Generator::StrIdx(llvm::StringRef Str)
{
    std::string S = Str;
    auto It = std::find(Strings.begin(), Strings.end(), S);
    if (It != Strings.end())
        return It - Strings.begin();
    Strings.push_back(std::move(S));
    return Strings.size() - 1;
}


