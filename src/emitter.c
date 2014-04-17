#include "../inc/emitter.h"

#include "../std/std.h"

#include "../inc/debug.h"
#include "../inc/type.h"
#include "../inc/ast.h"
#include "../inc/sym.h"
#include "../inc/architecture.h"
#include "../inc/ir.h"
#include "../inc/operand.h"
#include "../inc/asm.h"
#include "../inc/asm-amd64.h"
#include "../inc/reg.h"

#include "../inc/emitter-value.h"
#include "../inc/emitter-decl.h"
#include "../inc/emitter-helpers.h"

#include "string.h"
#include "stdlib.h"

static irBlock* emitterSetBreakTo (emitterCtx* ctx, irBlock* block);
static irBlock* emitterSetContinueTo (emitterCtx* ctx, irBlock* block);

static void emitterModule (emitterCtx* ctx, const ast* Node);
static void emitterFnImpl (emitterCtx* ctx, const ast* Node);

static void emitterCode (emitterCtx* ctx, irBlock* block, const ast* Node, irBlock* continuation);
static irBlock* emitterLine (emitterCtx* ctx, irBlock* block, const ast* Node);

static void emitterReturn (emitterCtx* ctx, irBlock* block, const ast* Node);

static irBlock* emitterBranch (emitterCtx* ctx, irBlock* block, const ast* Node);
static irBlock* emitterLoop (emitterCtx* ctx, irBlock* block, const ast* Node);
static irBlock* emitterIter (emitterCtx* ctx, irBlock* block, const ast* Node);

static emitterCtx* emitterInit (const char* output, const architecture* arch) {
    emitterCtx* ctx = malloc(sizeof(emitterCtx));
    ctx->ir = malloc(sizeof(irCtx));
    irInit(ctx->ir, output, arch);
    ctx->arch = arch;
    ctx->returnTo = 0;
    ctx->breakTo = 0;
    ctx->continueTo = 0;
    return ctx;
}

static void emitterEnd (emitterCtx* ctx) {
    irFree(ctx->ir);

    free(ctx->ir);
    free(ctx);
}

void emitter (const ast* Tree, const char* output, const architecture* arch) {
    emitterCtx* ctx = emitterInit(output, arch);

    emitterModule(ctx, Tree);
    irEmit(ctx->ir);

    emitterEnd(ctx);
}

static void emitterModule (emitterCtx* ctx, const ast* Node) {
    debugEnter("Module");

    for (ast* Current = Node->firstChild;
         Current;
         Current = Current->nextSibling) {
        if (Current->tag == astUsing) {
            if (Current->r)
                emitterModule(ctx, Current->r);

        } else if (Current->tag == astFnImpl)
            emitterFnImpl(ctx, Current);

        else if (Current->tag == astDecl)
            emitterDecl(ctx, Current);

        else if (Current->tag == astEmpty)
            debugMsg("Empty");

        else
            debugErrorUnhandled("emitterModule", "AST tag", astTagGetStr(Current->tag));
    }

    debugLeave();
}

static int emitterScopeAssignOffsets (const architecture* arch, sym* Scope, int offset) {
    for (int n = 0; n < Scope->children.length; n++) {
        sym* Symbol = vectorGet(&Scope->children, n);

        if (Symbol->tag == symScope)
            offset = emitterScopeAssignOffsets(arch, Symbol, offset);

        else if (Symbol->tag == symId) {
            offset -= typeGetSize(arch, Symbol->dt);
            Symbol->offset = offset;
            reportSymbol(Symbol);

        } else {}
    }

    return offset;
}

static void emitterFnImpl (emitterCtx* ctx, const ast* Node) {
    debugEnter("FnImpl");

    if (Node->symbol->label == 0)
        ctx->arch->symbolMangler(Node->symbol);

    /*Two words already on the stack:
      return ptr and saved base pointer*/
    int lastOffset = 2*ctx->arch->wordsize;

    /*Returning through temporary?*/
    if (typeGetSize(ctx->arch, typeGetReturn(Node->symbol->dt)) > ctx->arch->wordsize)
        lastOffset += ctx->arch->wordsize;

    /*Asign offsets to all the parameters*/
    for (int n = 0; n < Node->symbol->children.length; n++) {
        sym* Symbol = vectorGet(&Node->symbol->children, n);

        if (Symbol->tag != symParam)
            break;

        Symbol->offset = lastOffset;
        lastOffset += typeGetSize(ctx->arch, Symbol->dt);

        reportSymbol(Symbol);
    }

    /*Allocate stack space for all the auto variables
      Stack grows down, so the amount is the negation of the last offset*/
    int stacksize = -emitterScopeAssignOffsets(ctx->arch, Node->symbol, 0);

    /**/
    irBlock *block = irBlockCreate(ctx->ir),
            *epilogue = irBlockCreate(ctx->ir);

    ctx->returnTo = epilogue;

    irFnPrologue(block, Node->symbol->label, stacksize);
    emitterCode(ctx, block, Node->r, epilogue);
    irFnEpilogue(epilogue);

    debugLeave();
}

static void emitterCode (emitterCtx* ctx, irBlock* block, const ast* Node, irBlock* continuation) {
    for (ast* Current = Node->firstChild;
         Current;
         Current = Current->nextSibling) {
        block = emitterLine(ctx, block, Current);
    }

    irJump(block, continuation);
}

static irBlock* emitterLine (emitterCtx* ctx, irBlock* block, const ast* Node) {
    debugEnter(astTagGetStr(Node->tag));

    irBlock* continuation;

    if (Node->tag == astBranch)
        continuation = emitterBranch(ctx, block, Node);

    else if (Node->tag == astLoop)
        continuation = emitterLoop(ctx, block, Node);

    else if (Node->tag == astIter)
        continuation = emitterIter(ctx, block, Node);

    else if (Node->tag == astCode) {
        continuation = irBlockCreate(ctx->ir);
        emitterCode(ctx, block, Node, continuation);

    } else if (Node->tag == astReturn) {
        emitterReturn(ctx, block, Node);
        continuation = irBlockCreate(ctx->ir);

    } else if (Node->tag == astBreak) {
        irJump(block, ctx->breakTo);
        continuation = irBlockCreate(ctx->ir);

    } else if (Node->tag == astContinue) {
        irJump(block, ctx->continueTo);
        continuation = irBlockCreate(ctx->ir);

    } else if (Node->tag == astDecl) {
        emitterDecl(ctx, &block, Node);
        continuation = block;

    } else if (astIsValueTag(Node->tag)) {
        emitterValue(ctx, &block, Node, requestVoid);
        continuation = block;

    } else if (Node->tag == astEmpty)
        continuation = block;

    else
        debugErrorUnhandled("emitterLine", "AST tag", astTagGetStr(Node->tag));

    debugLeave();

    return continuation;
}

static void emitterReturn (emitterCtx* ctx, irBlock* block, const ast* Node) {
    /*Non void return?*/
    if (Node->r) {
        operand Ret = emitterValue(ctx, Node->r, requestValue);
        int retSize = typeGetSize(ctx->arch, Node->r->dt);

        bool retInTemp = retSize > ctx->arch->wordsize;

        /*Larger than word size ret => copy into caller allocated temporary pushed after args*/
        if (retInTemp) {
            operand tempRef = operandCreateReg(regAlloc(ctx->arch->wordsize));

            /*Dereference the temporary*/
            asmMove(ctx->Asm, tempRef, operandCreateMem(&regs[regRBP], 2*ctx->arch->wordsize, ctx->arch->wordsize));
            /*Copy over the value*/
            asmMove(ctx->Asm, operandCreateMem(tempRef.base, 0, retSize), Ret);
            operandFree(Ret);

            /*Return the temporary reference*/
            Ret = tempRef;
        }

        reg* rax;

        /*Returning either the return value itself or a reference to it*/
        if ((rax = regRequest(regRAX, retInTemp ? ctx->arch->wordsize : retSize)) != 0) {
            asmMove(ctx->Asm, operandCreateReg(rax), Ret);
            regFree(rax);

        } else if (Ret.base != regGet(regRAX))
            debugError("emitterLine", "unable to allocate RAX for return");

        operandFree(Ret);
    }

    irJump(block, ctx->returnTo);
}

static irBlock* emitterSetBreakTo (emitterCtx* ctx, irBlock* block) {
    irBlock* old = ctx->breakTo;
    ctx->breakTo = block;
    return old;
}

static irBlock* emitterSetContinueTo (emitterCtx* ctx, irBlock* block) {
    irBlock* old = ctx->continueTo;
    ctx->continueTo = block;
    return old;
}

static irBlock* emitterBranch (emitterCtx* ctx, irBlock* block, const ast* Node) {
    irBlock *continuation = irBlockCreate(ctx->ir),
            *ifTrue = irBlockCreate(ctx->ir),
            *ifFalse = irBlockCreate(ctx->ir);

    /*Condition, branch*/
    emitterBranchOnValue(ctx, block, Node->firstChild, ifTrue, ifFalse);

    /*Emit the true and false branches*/
    emitterCode(ctx, ifTrue, Node->l, continuation);
    emitterCode(ctx, ifFalse, Node->r, continuation);

    return continuation;
}

static irBlock* emitterLoop (emitterCtx* ctx, irBlock* block, const ast* Node) {
    irBlock *continuation = irBlockCreate(ctx->ir),
            *body = irBlockCreate(ctx->ir),
            *loopCheck = irBlockCreate(ctx->ir);

    /*Work out which order the condition and code came in
      => whether this is a while or a do while*/
    bool isDo = Node->l->tag == astCode;
    ast *cond = isDo ? Node->r : Node->l,
        *code = isDo ? Node->l : Node->r;

    /*A do while, no initial condition*/
    if (isDo)
        irJump(block, body);

    /*Initial condition: go into the body, or exit to the continuation*/
    else
        emitterBranchOnValue(ctx, block, cond, body, continuation);

    /*Loop body*/

    irBlock *oldBreakTo = emitterSetBreakTo(ctx, continuation),
            *oldContinueTo = emitterSetContinueTo(ctx, loopCheck);

    emitterCode(ctx, body, code, loopCheck);

    ctx->breakTo = oldBreakTo;
    ctx->continueTo = oldContinueTo;

    /*Loop re-entrant condition (in the loopCheck block this time)*/
    emitterBranchOnValue(ctx, loopCheck, cond, body, continuation);

    return continuation;
}

static irBlock* emitterIter (emitterCtx* ctx, irBlock* block, const ast* Node) {
    irBlock *continuation = irBlockCreate(ctx->ir),
            *body = irBlockCreate(ctx->ir),
            *iterate = irBlockCreate(ctx->ir);

    ast *init = Node->firstChild,
        *cond = init->nextSibling,
        *iter = cond->nextSibling,
        *code = Node->l;

    /*Initialization*/

    if (init->tag == astDecl)
        emitterDecl(ctx, &block, init);

    else
        emitterValue(ctx, &block, init, requestVoid);

    /*Condition*/
    emitterBranchOnValue(ctx, block, cond, body, continuation);

    /*Body*/

    irBlock* oldBreakTo = emitterSetBreakTo(ctx, continuation);
    irBlock* oldContinueTo = emitterSetContinueTo(ctx, iterate);

    emitterCode(ctx, body, code, iterate);

    ctx->breakTo = oldBreakTo;
    ctx->continueTo = oldContinueTo;

    /*Iterate and loop check*/
    emitterValue(ctx, &iterate, iter, requestVoid);
    emitterBranchOnValue(ctx, iterate, cond, body, continuation);

    return continuation;
}
