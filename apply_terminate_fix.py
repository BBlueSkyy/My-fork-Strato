#!/usr/bin/env python3
"""
Aplica o fix do TerminateHandler em 3 arquivos do fork do Strato.
Rode a partir da raiz do repo: python3 apply_terminate_fix.py
"""

import sys

SIGNAL_H = "app/src/main/cpp/skyline/common/signal.h"
SIGNAL_CPP = "app/src/main/cpp/skyline/common/signal.cpp"
EMU_JNI_CPP = "app/src/main/cpp/emu_jni.cpp"


def apply_edit(path, old, new, label):
    with open(path, "r", encoding="utf-8") as f:
        content = f.read()

    count = content.count(old)
    if count == 0:
        print(f"[FALHOU] {label}: texto original não encontrado em {path}")
        print("  --- Verifique se o arquivo já foi editado antes, ou se o caminho está certo ---")
        return False
    if count > 1:
        print(f"[FALHOU] {label}: texto original aparece {count} vezes em {path} (esperado 1) - edite manualmente")
        return False

    content = content.replace(old, new, 1)
    with open(path, "w", encoding="utf-8") as f:
        f.write(content)
    print(f"[OK] {label} aplicado em {path}")
    return True


def main():
    ok = True

    old_h = "    void ExceptionalSignalHandler(int signal, siginfo *, ucontext *context);"
    new_h = (
        "    void ExceptionalSignalHandler(int signal, siginfo *, ucontext *context);\n\n"
        "    /**\n"
        "     * @brief Handler de terminate customizado, instalado desde o início da execução\n"
        "     * pra evitar abort() silencioso em exceções não relacionadas a sinal\n"
        "     */\n"
        "    void TerminateHandler();"
    )
    ok &= apply_edit(SIGNAL_H, old_h, new_h, "1/4 - declaração em signal.h")

    old_cpp_1 = (
        "    void TerminateHandler() {\n"
        "        auto exception{std::current_exception()};\n"
        "        if (exception && exception == SignalExceptionPtr) {\n"
        "            StackFrame *frame;"
    )
    new_cpp_1 = (
        "    void TerminateHandler() {\n"
        "        auto exception{std::current_exception()};\n"
        "        if (exception && exception == SignalExceptionPtr) {\n"
        "            try {\n"
        "                std::rethrow_exception(exception);\n"
        "            } catch (const SignalException &e) {\n"
        "                LOGE(\"Terminating due to sinal sem catch handler na pilha: {}\", e.what());\n"
        "            }\n\n"
        "            StackFrame *frame;"
    )
    ok &= apply_edit(SIGNAL_CPP, old_cpp_1, new_cpp_1, "2/4 - log no ramo SignalException")

    old_cpp_2 = (
        "        } else {\n"
        "            SleepTillExit(); // We don't want to delegate to the older terminate handler as it might cause an exit\n"
        "        }\n"
        "    }"
    )
    new_cpp_2 = (
        "        } else {\n"
        "            if (exception) {\n"
        "                try {\n"
        "                    std::rethrow_exception(exception);\n"
        "                } catch (const std::exception &e) {\n"
        "                    LOGE(\"Terminating devido a exceção não capturada: {}\", e.what());\n"
        "                } catch (...) {\n"
        "                    LOGE(\"Terminating devido a exceção não capturada de tipo desconhecido\");\n"
        "                }\n"
        "            } else {\n"
        "                LOGE(\"std::terminate chamado sem exceção ativa\");\n"
        "            }\n"
        "            SleepTillExit(); // We don't want to delegate to the older terminate handler as it might cause an exit\n"
        "        }\n"
        "    }"
    )
    ok &= apply_edit(SIGNAL_CPP, old_cpp_2, new_cpp_2, "3/4 - log no ramo else")

    old_jni = '        skyline::signal::SetHostSignalHandler({SIGINT, SIGILL, SIGTRAP, SIGBUS, SIGFPE, SIGSEGV}, skyline::signal::ExceptionalSignalHandler);'
    new_jni = (
        '        skyline::signal::SetHostSignalHandler({SIGINT, SIGILL, SIGTRAP, SIGBUS, SIGFPE, SIGSEGV}, skyline::signal::ExceptionalSignalHandler);\n'
        '        std::set_terminate(skyline::signal::TerminateHandler); // Instala desde o início, não só após o primeiro sinal'
    )
    ok &= apply_edit(EMU_JNI_CPP, old_jni, new_jni, "4/4 - set_terminate em emu_jni.cpp")

    print()
    if ok:
        print("Todas as edições aplicadas. Confira com 'git diff' antes de commitar.")
    else:
        print("Uma ou mais edições falharam - confira as mensagens acima e edite manualmente onde precisar.")
        sys.exit(1)


if __name__ == "__main__":
    main()
