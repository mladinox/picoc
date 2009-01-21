#include <stdio.h>
#include <string.h>

#include "picoc.h"

/* the table of global definitions */
struct Table GlobalTable;
struct TableEntry GlobalHashTable[GLOBAL_TABLE_SIZE];

/* the table of function definitions */
struct LexState FunctionStore[FUNCTION_STORE_MAX];
int FunctionStoreUsed = 0;

/* the stack */
struct StackFrame Stack[STACK_MAX];
int StackUsed = 0;

/* parameter passing area */
struct Value Parameter[PARAMETER_MAX];
int ParameterUsed = 0;
struct Value ReturnValue;

/* local prototypes */
int ParseExpression(struct LexState *Lexer, struct Value *Result, int RunIt);
void ParseIntExpression(struct LexState *Lexer, struct Value *Result, int RunIt);
int ParseStatement(struct LexState *Lexer, int RunIt);
int ParseArguments(struct LexState *Lexer, int RunIt);


/* initialise the parser */
void ParseInit()
{
    TableInit(&GlobalTable, &GlobalHashTable[0], GLOBAL_TABLE_SIZE);
    IntrinsicInit(&GlobalTable);
}

/* define a variable */
void VariableDefine(struct LexState *Lexer, const Str *Ident, struct Value *InitValue)
{
    if (!TableSet((StackUsed == 0) ? &GlobalTable : &Stack[StackUsed-1].LocalTable, Ident, InitValue))
        ProgramFail(Lexer, "'%S' is already defined", Ident);
}

/* get the value of a variable. must be defined */
void VariableGet(struct LexState *Lexer, Str *Ident, struct Value *Val, struct Value **LVal)
{
    if (StackUsed == 0 || !TableGet(&Stack[StackUsed-1].LocalTable, Ident, LVal))
    {
        if (!TableGet(&GlobalTable, Ident, LVal))
            ProgramFail(Lexer, "'%S' is undefined", Ident);
    }

    *Val = **LVal;
}

/* add a stack frame when doing a function call */
void StackFrameAdd(struct LexState *Lexer)
{
    struct StackFrame *NewFrame = &Stack[StackUsed];
    
    if (StackUsed >= STACK_MAX)
        ProgramFail(Lexer, "too many nested function calls");
        
    NewFrame->ReturnLex = *Lexer;
    TableInit(&NewFrame->LocalTable, &NewFrame->LocalHashTable[0], LOCAL_TABLE_SIZE);
    StackUsed++;
}

/* parse a type specification */
int ParseType(struct LexState *Lexer, enum ValueType *Typ)
{
    struct LexState Before = *Lexer;
    enum LexToken Token = LexGetPlainToken(Lexer);
    switch (Token)
    {
        case TokenIntType: case TokenCharType: *Typ = TypeInt; return TRUE;
        case TokenFloatType: case TokenDoubleType: *Typ = TypeFP; return TRUE;
        case TokenVoidType: *Typ = TypeVoid; return TRUE;
        default: *Lexer = Before; return FALSE;
    }
}

/* parse a parameter list, defining parameters as local variables in the current scope */
void ParseParameterList(struct LexState *CallLexer, struct LexState *FuncLexer, int RunIt)
{
    enum ValueType Typ;
    union AnyValue Identifier;
    enum LexToken Token = LexGetPlainToken(FuncLexer);  /* open bracket */
    int ParamCount;
    
    for (ParamCount = 0; ParamCount < ParameterUsed; ParamCount++)
    {
        ParseType(FuncLexer, &Typ);
        Token = LexGetToken(FuncLexer, &Identifier);
        if (Token != TokenComma && Token != TokenCloseBracket)
        {   /* there's an identifier */
            if (Token != TokenIdentifier)
                ProgramFail(FuncLexer, "invalid parameter");
                
            if (RunIt)
            {
                if (Parameter[ParamCount].Typ != Typ)
                    ProgramFail(CallLexer, "parameter %d has the wrong type", ParamCount+1);
                    
                VariableDefine(FuncLexer, &Identifier.String, &Parameter[ParamCount]);
            }
    
            Token = LexGetPlainToken(FuncLexer);
            if (Token != TokenComma && Token != TokenCloseBracket)
                ProgramFail(FuncLexer, "comma expected");
        }
    } 
    if (ParameterUsed == 0)
        Token = LexGetPlainToken(FuncLexer);
    
    if (Token != TokenCloseBracket)
        ProgramFail(CallLexer, "wrong number of arguments");
}

/* do a function call */
void ParseFunctionCall(struct LexState *Lexer, struct Value *Result, Str *FuncName, int RunIt)
{
    enum LexToken Token = LexGetPlainToken(Lexer);    /* open bracket */
    
    /* parse arguments */
    ParameterUsed = 0;
    do {
        if (ParseExpression(Lexer, &Parameter[ParameterUsed], RunIt))
        {
            if (RunIt && ParameterUsed >= PARAMETER_MAX)
                ProgramFail(Lexer, "too many arguments");
                
            ParameterUsed++;
            Token = LexGetPlainToken(Lexer);
            if (Token != TokenComma && Token != TokenCloseBracket)
                ProgramFail(Lexer, "comma expected");
        }
        else
        {
            Token = LexGetPlainToken(Lexer);
            if (!TokenCloseBracket)
                ProgramFail(Lexer, "bad argument");
        }
    } while (Token != TokenCloseBracket);
    
    if (RunIt) 
    {
        struct LexState FuncLexer;
        enum ValueType ReturnType;
        struct Value *LValue;
        
        VariableGet(Lexer, FuncName, Result, &LValue);
        if (Result->Typ != TypeFunction)
            ProgramFail(Lexer, "not a function - can't call");
        
        StackFrameAdd(Lexer);
        if (Result->Val.Integer >= 0)
            FuncLexer = FunctionStore[Result->Val.Integer];
        else
            IntrinsicGetLexer(&FuncLexer, Result->Val.Integer);

        ParseType(&FuncLexer, &ReturnType);             /* return type */
        Result->Typ = TypeVoid;
        LexGetPlainToken(&FuncLexer);                   /* function name again */
        ParseParameterList(Lexer, &FuncLexer, TRUE);    /* parameters */
        if (Result->Val.Integer >= 0)
        {
            if (LexPeekPlainToken(&FuncLexer) != TokenLeftBrace || !ParseStatement(&FuncLexer, TRUE))
                ProgramFail(&FuncLexer, "function body expected");
        
            if (ReturnType != Result->Typ)
                ProgramFail(&FuncLexer, "bad return value");
        }
        else
            IntrinsicCall(Lexer, Result, ReturnType, Result->Val.Integer);
    }
}

/* parse a single value */
int ParseValue(struct LexState *Lexer, struct Value *Result, struct Value **LValue, int RunIt)
{
    struct LexState PreState = *Lexer;
    enum LexToken Token = LexGetToken(Lexer, &Result->Val);
    *LValue = NULL;
    
    switch (Token)
    {
        case TokenIntegerConstant: case TokenCharacterConstant: Result->Typ = TypeInt; break;
        case TokenFPConstant: Result->Typ = TypeFP; break;
        case TokenStringConstant: Result->Typ = TypeString; break;
        
        case TokenMinus:  case TokenUnaryExor: case TokenUnaryNot:
            ParseIntExpression(Lexer, Result, RunIt);
                
            if (RunIt)
            {
                switch(Token)
                {
                    case TokenMinus: Result->Val.Integer = -(Result->Val.Integer); break;
                    case TokenUnaryExor: Result->Val.Integer = ~(Result->Val.Integer); break;
                    case TokenUnaryNot: Result->Val.Integer = !(Result->Val.Integer); break;
                    default: break;
                }
            }
            break;

        case TokenOpenBracket:
            if (!ParseExpression(Lexer, Result, RunIt))
                ProgramFail(Lexer, "invalid expression");
            
            if (LexGetPlainToken(Lexer) != TokenCloseBracket)
                ProgramFail(Lexer, "')' expected");
            break;
                
        case TokenAsterisk:
        case TokenAmpersand:
            ProgramFail(Lexer, "not implemented");

        case TokenIdentifier:
            if (LexPeekPlainToken(Lexer) == TokenOpenBracket)
                ParseFunctionCall(Lexer, Result, &Result->Val.String, RunIt);
            else
            {
                if (RunIt)
                {
                    VariableGet(Lexer, &Result->Val.String, Result, LValue);
                    if (Result->Typ == TypeMacro)
                    {
                        struct LexState MacroLexer = FunctionStore[Result->Val.Integer];
                        
                        if (!ParseExpression(&MacroLexer, Result, TRUE))
                            ProgramFail(&MacroLexer, "expression expected");
                    }
                    else if (!ISVALUETYPE(Result->Typ))
                        ProgramFail(Lexer, "bad variable type");
                }
            }
            break;
            
        default:
            *Lexer = PreState;
            return FALSE;
    }
    
    return TRUE;
}

/* parse an expression. operator precedence is not supported */
int ParseExpression(struct LexState *Lexer, struct Value *Result, int RunIt)
{
    struct Value CurrentValue;
    struct Value *CurrentLValue;
    struct Value TotalValue;
    struct Value *TotalLValue;
    
    if (!ParseValue(Lexer, &TotalValue, &TotalLValue, RunIt))
        return FALSE;
    
    while (TRUE)
    {
        enum LexToken Token = LexPeekToken(Lexer, &CurrentValue.Val);
        switch (Token)
        {
            case TokenPlus: case TokenMinus: case TokenAsterisk: case TokenSlash:
            case TokenEquality: case TokenLessThan: case TokenGreaterThan:
            case TokenLessEqual: case TokenGreaterEqual: case TokenLogicalAnd:
            case TokenLogicalOr: case TokenAmpersand: case TokenArithmeticOr: 
            case TokenArithmeticExor: case TokenDot:
                LexGetPlainToken(Lexer);
                break;
                        
            case TokenAssign: case TokenAddAssign: case TokenSubtractAssign:
                LexGetPlainToken(Lexer);
                if (!ParseExpression(Lexer, &CurrentValue, RunIt))
                    ProgramFail(Lexer, "expression expected");
                
                if (RunIt)
                {
                    if (CurrentValue.Typ != TypeInt || TotalValue.Typ != TypeInt)
                        ProgramFail(Lexer, "can't assign");

                    switch (Token)
                    {
                        case TokenAddAssign: TotalValue.Val.Integer += CurrentValue.Val.Integer; break;
                        case TokenSubtractAssign: TotalValue.Val.Integer -= CurrentValue.Val.Integer; break;
                        default: TotalValue.Val.Integer = CurrentValue.Val.Integer; break;
                    }
                    *TotalLValue = TotalValue;
                }
                // fallthrough
            
            default:
                if (RunIt)
                    *Result = TotalValue;
                return TRUE;
        }
        
        if (!ParseValue(Lexer, &CurrentValue, &CurrentLValue, RunIt))
            return FALSE;

        if (RunIt)
        {
            if (CurrentValue.Typ == TypeFP || TotalValue.Typ == TypeFP)
            {   /* convert both to floating point */
                if (CurrentValue.Typ == TypeInt)
                    CurrentValue.Val.FP = (double)CurrentValue.Val.Integer;
                else if (CurrentValue.Typ != TypeFP)
                    ProgramFail(Lexer, "bad type for operator");
                    
                if (TotalValue.Typ == TypeInt)
                    TotalValue.Val.FP = (double)TotalValue.Val.Integer;
                else if (TotalValue.Typ != TypeFP)
                    ProgramFail(Lexer, "bad type for operator");

                TotalValue.Typ = TypeInt;
                switch (Token)
                {
                    case TokenPlus: TotalValue.Val.FP += CurrentValue.Val.FP; TotalValue.Typ = TypeFP; break;
                    case TokenMinus: TotalValue.Val.FP -= CurrentValue.Val.FP; TotalValue.Typ = TypeFP; break;
                    case TokenAsterisk: TotalValue.Val.FP *= CurrentValue.Val.FP; TotalValue.Typ = TypeFP; break;
                    case TokenSlash: TotalValue.Val.FP /= CurrentValue.Val.FP; TotalValue.Typ = TypeFP; break;
                    case TokenEquality: TotalValue.Val.Integer = TotalValue.Val.FP == CurrentValue.Val.FP; break;
                    case TokenLessThan: TotalValue.Val.Integer = TotalValue.Val.FP < CurrentValue.Val.FP; break;
                    case TokenGreaterThan: TotalValue.Val.Integer = TotalValue.Val.FP > CurrentValue.Val.FP; break;
                    case TokenLessEqual: TotalValue.Val.Integer = TotalValue.Val.FP <= CurrentValue.Val.FP; break;
                    case TokenGreaterEqual: TotalValue.Val.Integer = TotalValue.Val.FP >= CurrentValue.Val.FP; break;
                    case TokenLogicalAnd: case TokenLogicalOr: case TokenAmpersand: case TokenArithmeticOr: case TokenArithmeticExor: ProgramFail(Lexer, "bad type for operator"); break;
                    case TokenDot: ProgramFail(Lexer, "operator not supported"); break;
                    default: break;
                }
            }
            else
            {
                if (CurrentValue.Typ != TypeInt || TotalValue.Typ != TypeInt)
                    ProgramFail(Lexer, "bad operand types");
            
                /* integer arithmetic */
                switch (Token)
                {
                    case TokenPlus: TotalValue.Val.Integer += CurrentValue.Val.Integer; break;
                    case TokenMinus: TotalValue.Val.Integer -= CurrentValue.Val.Integer; break;
                    case TokenAsterisk: TotalValue.Val.Integer *= CurrentValue.Val.Integer; break;
                    case TokenSlash: TotalValue.Val.Integer /= CurrentValue.Val.Integer; break;
                    case TokenEquality: TotalValue.Val.Integer = TotalValue.Val.Integer == CurrentValue.Val.Integer; break;
                    case TokenLessThan: TotalValue.Val.Integer = TotalValue.Val.Integer < CurrentValue.Val.Integer; break;
                    case TokenGreaterThan: TotalValue.Val.Integer = TotalValue.Val.Integer > CurrentValue.Val.Integer; break;
                    case TokenLessEqual: TotalValue.Val.Integer = TotalValue.Val.Integer <= CurrentValue.Val.Integer; break;
                    case TokenGreaterEqual: TotalValue.Val.Integer = TotalValue.Val.Integer >= CurrentValue.Val.Integer; break;
                    case TokenLogicalAnd: TotalValue.Val.Integer = TotalValue.Val.Integer && CurrentValue.Val.Integer; break;
                    case TokenLogicalOr: TotalValue.Val.Integer = TotalValue.Val.Integer || CurrentValue.Val.Integer; break;
                    case TokenAmpersand: TotalValue.Val.Integer = TotalValue.Val.Integer & CurrentValue.Val.Integer; break;
                    case TokenArithmeticOr: TotalValue.Val.Integer = TotalValue.Val.Integer | CurrentValue.Val.Integer; break;
                    case TokenArithmeticExor: TotalValue.Val.Integer = TotalValue.Val.Integer ^ CurrentValue.Val.Integer; break;
                    case TokenDot: ProgramFail(Lexer, "operator not supported"); break;
                    default: break;
                }
            }
        }
    }
    
    return TRUE;
}

/* parse an expression. operator precedence is not supported */
void ParseIntExpression(struct LexState *Lexer, struct Value *Result, int RunIt)
{
    if (!ParseExpression(Lexer, Result, RunIt))
        ProgramFail(Lexer, "expression expected");
    
    if (RunIt && Result->Typ != TypeInt)
        ProgramFail(Lexer, "integer value expected");
}

/* parse a function definition and store it for later */
void ParseFunctionDefinition(struct LexState *Lexer, Str *Identifier, struct LexState *PreState)
{
    struct Value FuncValue;

    if (FunctionStoreUsed >= FUNCTION_STORE_MAX)
        ProgramFail(Lexer, "too many functions/macros defined");
    FunctionStore[FunctionStoreUsed] = *PreState;
    
    LexGetPlainToken(Lexer);
    if (LexGetPlainToken(Lexer) != TokenCloseBracket || LexPeekToken(Lexer, &FuncValue.Val) != TokenLeftBrace)
        ProgramFail(Lexer, "bad function definition");
    
    if (!ParseStatement(Lexer, FALSE))
        ProgramFail(Lexer, "function definition expected");
    
    FunctionStore[FunctionStoreUsed].End = Lexer->Pos;
    FuncValue.Typ = TypeFunction;
    FuncValue.Val.Integer = FunctionStoreUsed;
    FunctionStoreUsed++;
    
    if (!TableSet(&GlobalTable, Identifier, &FuncValue))
        ProgramFail(Lexer, "'%S' is already defined", Identifier);
}

/* parse a #define macro definition and store it for later */
void ParseMacroDefinition(struct LexState *Lexer)
{
    union AnyValue MacroName;
    struct Value MacroValue;

    if (LexGetToken(Lexer, &MacroName) != TokenIdentifier)
        ProgramFail(Lexer, "identifier expected");
    
    if (FunctionStoreUsed >= FUNCTION_STORE_MAX)
        ProgramFail(Lexer, "too many functions/macros defined");

    FunctionStore[FunctionStoreUsed] = *Lexer;
    LexToEndOfLine(Lexer);
    FunctionStore[FunctionStoreUsed].End = Lexer->Pos;
    MacroValue.Typ = TypeMacro;
    MacroValue.Val.Integer = FunctionStoreUsed;
    FunctionStoreUsed++;
    
    if (!TableSet(&GlobalTable, &MacroName.String, &MacroValue))
        ProgramFail(Lexer, "'%S' is already defined", &MacroName.String);
}

void ParseFor(struct LexState *Lexer, struct Value *Result, int RunIt)
{
    struct Value Conditional;
    struct LexState PreConditional;
    struct LexState PreIncrement;
    struct LexState PreStatement;
    struct LexState After;

    if (LexGetPlainToken(Lexer) != TokenOpenBracket)
        ProgramFail(Lexer, "'(' expected");
                        
    if (!ParseStatement(Lexer, RunIt))
        ProgramFail(Lexer, "statement expected");
    
    PreConditional = *Lexer;
    ParseIntExpression(Lexer, &Conditional, RunIt);
    
    if (LexGetPlainToken(Lexer) != TokenSemicolon)
        ProgramFail(Lexer, "';' expected");
    
    PreIncrement = *Lexer;
    ParseStatement(Lexer, FALSE);
    
    if (LexGetPlainToken(Lexer) != TokenCloseBracket)
        ProgramFail(Lexer, "')' expected");
    
    PreStatement = *Lexer;
    if (!ParseStatement(Lexer, RunIt && Conditional.Val.Integer))
        ProgramFail(Lexer, "statement expected");
    
    After = *Lexer;
    
    while (Conditional.Val.Integer && RunIt)
    {
        *Lexer = PreIncrement;
        ParseStatement(Lexer, TRUE);
                        
        *Lexer = PreConditional;
        ParseIntExpression(Lexer, &Conditional, RunIt);
        
        if (Conditional.Val.Integer)
        {
            *Lexer = PreStatement;
            ParseStatement(Lexer, TRUE);
        }
    }
    
    *Lexer = After;
}

/* parse a statement */
int ParseStatement(struct LexState *Lexer, int RunIt)
{
    struct Value CValue;
    struct LexState PreState = *Lexer;
    union AnyValue LexerValue;
    enum ValueType Typ;
    enum LexToken Token = LexGetToken(Lexer, &LexerValue);
    
    switch (Token)
    {
        case TokenEOF:
            return FALSE;
            
        case TokenIdentifier: 
            *Lexer = PreState;
            ParseExpression(Lexer, &CValue, RunIt);
            break;
            
        case TokenLeftBrace:
            while (ParseStatement(Lexer, RunIt))
            {}
            
            if (LexGetPlainToken(Lexer) != TokenRightBrace)
                ProgramFail(Lexer, "'}' expected");
            break;
            
        case TokenIf:
            ParseIntExpression(Lexer, &CValue, RunIt);
            
            if (!ParseStatement(Lexer, RunIt && CValue.Val.Integer))
                ProgramFail(Lexer, "statement expected");
            
            if (LexPeekToken(Lexer, &LexerValue) == TokenElse)
            {
                LexGetToken(Lexer, &LexerValue);
                if (!ParseStatement(Lexer, RunIt && !CValue.Val.Integer))
                    ProgramFail(Lexer, "statement expected");
            }
            break;
        
        case TokenWhile:
            {
                struct LexState PreConditional = *Lexer;
                do
                {
                    *Lexer = PreConditional;
                    ParseIntExpression(Lexer, &CValue, RunIt);
                
                    if (!ParseStatement(Lexer, RunIt && CValue.Val.Integer))
                        ProgramFail(Lexer, "statement expected");
                        
                } while (RunIt && CValue.Val.Integer);                
            }
            break;
                
        case TokenDo:
            {
                struct LexState PreStatement = *Lexer;
                do
                {
                    *Lexer = PreStatement;
                    if (!ParseStatement(Lexer, RunIt))
                        ProgramFail(Lexer, "statement expected");
                        
                    ParseIntExpression(Lexer, &CValue, RunIt);
                
                } while (CValue.Val.Integer && RunIt);           
            }
            break;
                
        case TokenFor:
            break;

        case TokenSemicolon: break;

        case TokenIntType:
        case TokenCharType:
        case TokenFloatType:
        case TokenDoubleType:
        case TokenVoidType:
            *Lexer = PreState;
            ParseType(Lexer, &Typ);
            if (LexGetToken(Lexer, &LexerValue) != TokenIdentifier)
                ProgramFail(Lexer, "identifier expected");
                
            /* handle function definitions */
            if (LexPeekPlainToken(Lexer) == TokenOpenBracket)
                ParseFunctionDefinition(Lexer, &LexerValue.String, &PreState);
            else
            {
                struct Value InitValue;
                if (Typ == TokenFloatType)
                    InitValue.Val.FP = 0.0;
                else
                    InitValue.Val.Integer = 0;
                    
                InitValue.Typ = Typ;
                VariableDefine(Lexer, &LexerValue.String, &InitValue);
            }
            break;
        
        case TokenHashDefine:
            ParseMacroDefinition(Lexer);
            break;
            
        case TokenHashInclude:
            if (LexGetToken(Lexer, &LexerValue) != TokenStringConstant)
                ProgramFail(Lexer, "\"filename.h\" expected");
            
            ScanFile(&LexerValue.String);
            LexToEndOfLine(Lexer);
            break;

        case TokenSwitch:
        case TokenCase:
        case TokenBreak:
        case TokenReturn:
        case TokenDefault:
            ProgramFail(Lexer, "not implemented yet");
            break;
            
        default:
            *Lexer = PreState;
            return FALSE;
    }
    
    return TRUE;
}

/* quick scan a source file for definitions */
void Parse(const Str *FileName, const Str *Source, int RunIt)
{
    struct LexState Lexer;
    
    LexInit(&Lexer, Source, FileName, 1);
    
    while (ParseStatement(&Lexer, RunIt))
    {}
    
    if (Lexer.Pos != Lexer.End)
        ProgramFail(&Lexer, "parse error");
}
