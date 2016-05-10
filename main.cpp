#include "raven.h"

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    Raven raven(argv[1]);
    raven << Raven::tag("test", "test tag");
    QObject::connect(&raven, &Raven::eventSent, &app, &QCoreApplication::quit);
    raven(RAVEN_INFO)
        << QString("This is a test message generated using ``raven test``")
        << Raven::send;
    return app.exec();
}
