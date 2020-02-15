//
// Created by kiva on 2018/3/28.
//
#include <kivm/runtime/javaThread.h>
#include <kivm/bytecode/execution.h>
#include <kivm/bytecode/javaCall.h>
#include <kivm/oop/primitiveOop.h>
#include <kivm/oop/arrayKlass.h>
#include <kivm/oop/arrayOop.h>
#include <kivm/native/classNames.h>
#include <kivm/native/java_lang_Class.h>
#include <kivm/native/java_lang_Thread.h>
#include <kivm/native/java_lang_String.h>
#include <kivm/native/java_lang_reflect_Constructor.h>
#include <kivm/native/java_lang_reflect_Method.h>

namespace kivm {
    static inline InstanceKlass *use(ClassLoader *cl, JavaMainThread *thread, const String &name) {
        auto klass = (InstanceKlass *) cl->loadClass(name);
        if (klass == nullptr) {
            PANIC("java.lang.LinkError: class not found: %S",
                (name).c_str());
        }
        if (!Execution::initializeClass(thread, klass)) {
            PANIC("class init failed");
        }
        return klass;
    }

    JavaMainThread::JavaMainThread(const String &mainClassName, const std::vector<String> &arguments)
        : JavaThread(nullptr, {}),
          _mainClassName(mainClassName), _arguments(arguments),
          _classFromStream(false), _mainClassBytes(nullptr) {
    }

    JavaMainThread::JavaMainThread(u1 *mainClassBytes, size_t classSize, const std::vector<String> &arguments)
        : JavaThread(nullptr, {}),
          _mainClassName(), _arguments(arguments),
          _classFromStream(true), _mainClassBytes(mainClassBytes), _mainClassSize(classSize) {
    }

    void JavaMainThread::run() {
        setThreadName(L"JavaMainThread");

        // Initialize Java Virtual Machine
        Threads::initializeJVM(this);

        D("Threads::initializeJVM() succeeded. Lunching main()");

        InstanceKlass *mainClass = nullptr;
        if (_classFromStream) {
            mainClass = (InstanceKlass *) BootstrapClassLoader::get()->loadClass(_mainClassBytes, _mainClassSize);
        } else {
            mainClass = (InstanceKlass *) BootstrapClassLoader::get()->loadClass(_mainClassName);
        }
        
        if (mainClass == nullptr) {
            PANIC("class not found: %S", (_mainClassName).c_str());
        }

        auto mainMethod = mainClass->getStaticMethod(L"main", L"([Ljava/lang/String;)V");
        if (mainMethod == nullptr) {
            PANIC("method main(String[]) not found in %S",
                (_mainClassName).c_str());
        }

        auto stringArrayClass = (ObjectArrayKlass *) BootstrapClassLoader::get()->loadClass(L"[Ljava/lang/String;");
        auto argumentArrayOop = stringArrayClass->newInstance((int) _arguments.size());

        for (int i = 0; i < argumentArrayOop->getLength(); ++i) {
            auto stringOop = java::lang::String::intern(_arguments[i]);
            argumentArrayOop->setElementAt(i, stringOop);
        }

        this->_method = mainMethod;
        this->_args.push_back(argumentArrayOop);
        JavaCall::withArgs(this, _method, _args);
    }

    void JavaMainThread::onThreadLaunched() {
        // Start the first app thread to run main(String[])
        this->_nativeThread->join();

        // Then, let's wait for all app threads to finish
        for (;;) {
            int threads = Threads::getRunningJavaThreadCountLocked();
            assert(threads >= 0);


            if (threads == 0) {
                D("no remaining java thread, exiting...");
                break;
            }

            sched_yield();
        }
    }

    void Threads::initializeJVM(JavaMainThread *thread) {
        if (Global::jvmBooted) {
            return;
        }

        auto cl = BootstrapClassLoader::get();
       
        
        Threads::initializeVMStructs(cl, thread);

        use(cl, thread, J_STRING);

        
        
        auto threadClass = use(cl, thread, J_THREAD);
        auto threadGroupClass = use(cl, thread, J_THREAD_GROUP);
        
        
        
        // Create the init thread
        JavaObject("Thread") initThreadOop = threadClass->newInstance();
        // eetop is a pointer to the underlying OS-level native thread instance of the JVM.
        initThreadOop->setFieldValue(J_THREAD, L"eetop", L"J",
            new longOopDesc(thread->getNativeHandler()));
        initThreadOop->setFieldValue(J_THREAD, L"priority", L"I",
            new intOopDesc(java::lang::ThreadPriority::NORMAL_PRIORITY));

        // JavaMainThread is created with javaThreadObject == nullptr
        // Now we have created a thread for it.
        thread->setJavaThreadObject(initThreadOop);

        // do not addJavaThread again
        // Threads::addJavaThread(thread);

        // Create and construct the system thread group.
        JavaObject("ThreadGroup") systemThreadGroup = threadGroupClass->newInstance();
        auto tgDefaultCtor = threadGroupClass->getThisClassMethod(L"<init>", L"()V");
        JavaCall::withArgs(thread, tgDefaultCtor, {systemThreadGroup});

        // Create the main thread group
        JavaObject("ThreadGroup") mainThreadGroup = threadGroupClass->newInstance();
        initThreadOop->setFieldValue(J_THREAD, L"group", L"Ljava/lang/ThreadGroup;", mainThreadGroup);

        // Load system classes.
        auto systemClass = (InstanceKlass *) cl->loadClass(L"java/lang/System");
        systemClass->setClassState(ClassState::BEING_INITIALIZED);
        use(cl, thread, J_INPUT_STREAM);
        use(cl, thread, J_PRINT_STREAM);
        use(cl, thread, J_SECURITY_MANAGER);

        // Construct the main thread group
        // use getThisClassMethod() to get a private method
        auto threadGroupCtor = threadGroupClass->getThisClassMethod(L"<init>",
            L"(Ljava/lang/Void;Ljava/lang/ThreadGroup;Ljava/lang/String;)V");
        {
            std::list<oop> args;
            args.push_back(mainThreadGroup);
            // we need to push `nullptr` into list
            // so do not use something like {nullptr, ...}
            args.push_back(nullptr);
            args.push_back(systemThreadGroup);
            args.push_back(java::lang::String::intern(L"main"));
            JavaCall::withArgs(thread, threadGroupCtor, args);
        }


        // disable sun.security.util.Debug for the following operations
        auto sunDebugClass = cl->loadClass(L"sun/security/util/Debug");
        sunDebugClass->setClassState(ClassState::BEING_INITIALIZED);

        // Construct the init thread by attaching the main thread group to it.
        auto threadCtor = threadClass->getThisClassMethod(L"<init>",
            L"(Ljava/lang/ThreadGroup;Ljava/lang/String;)V");
        JavaCall::withArgs(thread, threadCtor,
            {initThreadOop, mainThreadGroup, java::lang::String::intern(L"main")});

        Threads::hackJavaClasses(cl, thread);

        // Initialize system classes.
        auto initSystemClassesMethod = systemClass->getStaticMethod(L"initializeSystemClass", L"()V");
        JavaCall::withArgs(thread, initSystemClassesMethod, {});

        // re-enable sun.security.util.Debug
        sunDebugClass->setClassState(ClassState::FULLY_INITIALIZED);
        InstanceKlass* bscl = use(cl, thread, J_KiVMBSCL);
        if (bscl != nullptr){
            D("Found KiVMBootstrapClassLoader");
            
            JavaObject("KiVMBootstrapClassLoader") jcl = bscl->newInstance();
            Method* constructor = bscl->getThisClassMethod(L"<init>", L"([Ljava/lang/String;)V");
            
            auto stringArrayClass = (ObjectArrayKlass *) BootstrapClassLoader::get()->loadClass(L"[Ljava/lang/String;");
            
            
            
            auto argumentArrayOop = stringArrayClass->newInstance((int) _arguments.size());

            for (int i = 0; i < argumentArrayOop->getLength(); ++i) {
                auto stringOop = java::lang::String::intern(_arguments[i]);
                argumentArrayOop->setElementAt(i, stringOop);
            }
            
            JavaCall::withArgs(thread, constructor, {jcl, argumentArrayOop});
            
            cl->jKiVMBootstrapClassloader = jcl;
            
        }
        
        Global::jvmBooted = true;
    }

    void Threads::initializeVMStructs(BootstrapClassLoader *cl, JavaMainThread *thread) {
        java::lang::Class::initialize();
        auto classClass = use(cl, thread, J_CLASS);
        java::lang::Class::mirrorCoreAndDelayedClasses();
        java::lang::Class::mirrorDelayedArrayClasses();
        Global::_Object = use(cl, thread, J_OBJECT);
        Global::_Class = classClass;
        Global::_String = use(cl, thread, J_STRING);
        Global::_Cloneable = use(cl, thread, J_CLONEABLE);
        Global::_Serializable = use(cl, thread, J_SERIALIZABLE);
        Global::_NullPointerException = use(cl, thread, J_NPE);
        Global::_ArrayIndexOutOfBoundsException = use(cl, thread, J_ARRAY_INDEX_OUT_OF_BOUNDS);
        Global::_ClassNotFoundException = use(cl, thread, J_CLASS_NOT_FOUND);
        Global::_InternalError = use(cl, thread, J_INTERNAL_ERROR);
        Global::_IOException = use(cl, thread, J_IOEXCEPTION);
        java::lang::reflect::Constructor::initialize();
        java::lang::reflect::Method::initialize();

        classClass->setStaticFieldValue(J_CLASS, L"useCaches", L"Z", new intOopDesc(false));
    }

    void Threads::hackJavaClasses(BootstrapClassLoader *cl, JavaMainThread *thread) {
        // TODO: java.nio.charset.Charset.forName() cannot find any charsets
        // hack java.nio.charset.Charset.defaultCharset
        auto charsetClass = (InstanceKlass *) BootstrapClassLoader::get()
            ->loadClass(L"java/nio/charset/Charset");
        assert(Execution::initializeClass(thread, charsetClass));
        auto utf8CharsetClass = (InstanceKlass *) BootstrapClassLoader::get()
            ->loadClass(L"sun/nio/cs/UTF_8");
        assert(Execution::initializeClass(thread, utf8CharsetClass));
        Global::DEFAULT_UTF8_CHARSET = utf8CharsetClass->newInstance();
        charsetClass->setStaticFieldValue(charsetClass->getName(),
            L"defaultCharset", L"Ljava/nio/charset/Charset;",
            Global::DEFAULT_UTF8_CHARSET);

        // The extremely powerful magic
        auto encoder = (InstanceKlass *) BootstrapClassLoader::get()
            ->loadClass(L"sun/nio/cs/StreamEncoder");
        auto method = encoder->getStaticMethod(L"forOutputStreamWriter",
            L"(Ljava/io/OutputStream;Ljava/lang/Object;Ljava/lang/String;)Lsun/nio/cs/StreamEncoder;");
        method->hackAsNative();

        // TODO: support System.load() and System.loadLibrary()
        auto systemClass = (InstanceKlass *) BootstrapClassLoader::get()
            ->loadClass(L"java/lang/System");
        auto loadMethod = systemClass->getStaticMethod(L"load", L"(Ljava/lang/String;)V");
        loadMethod->hackAsNative();
        auto loadLibraryMethod = systemClass->getStaticMethod(L"loadLibrary", L"(Ljava/lang/String;)V");
        loadLibraryMethod->hackAsNative();
    }
}
