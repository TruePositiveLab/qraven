#include "raven.h"

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    Raven raven(argv[1]);
    QObject::connect(&raven, &Raven::eventSent, &app, &QCoreApplication::quit);
    raven(RAVEN_INFO)
        << QString("This is a test message generated using ``raven test``")
        << RavenTag("test", "test tag") << RavenEnd();
    return app.exec();
}
