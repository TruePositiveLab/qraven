#include "raven.h"

void test_throw() { throw std::runtime_error("test exception"); }

int main(int argc, char** argv)
{
    qDebug() << argc;
    QCoreApplication app(argc, argv);

    Raven raven(argv[1]);
    /*raven << Raven::tag("test", "test tag");*/
    // raven(RAVEN_INFO)
    //<< QString("This is a test message generated using ``raven test``")
    /*<< Raven::send;*/
    QObject::connect(&raven, &Raven::eventSent, &app, &QCoreApplication::quit);
    try {
        test_throw();
    }
    catch (const std::runtime_error& exc) {
        raven(RAVEN_ERROR) << exc << Raven::send;
    }
    return app.exec();
}
