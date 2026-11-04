#include "MainComponent.h"
#include "Theme.h"

class SecantApplication : public juce::JUCEApplication
{
public:
    const juce::String getApplicationName() override { return JUCE_APPLICATION_NAME_STRING; }
    const juce::String getApplicationVersion() override { return JUCE_APPLICATION_VERSION_STRING; }
    bool moreThanOneInstanceAllowed() override { return true; }

    void initialise (const juce::String&) override
    {
        theme = std::make_unique<ui::Theme>();
        juce::LookAndFeel::setDefaultLookAndFeel (theme.get());
        mainWindow = std::make_unique<MainWindow> (getApplicationName());
    }
    void shutdown() override
    {
        mainWindow = nullptr;
        juce::LookAndFeel::setDefaultLookAndFeel (nullptr);
        theme = nullptr;
    }
    void systemRequestedQuit() override { quit(); }

    class MainWindow : public juce::DocumentWindow
    {
    public:
        explicit MainWindow (const juce::String& name)
            : DocumentWindow (name, ui::colours().background, DocumentWindow::allButtons)
        {
            setUsingNativeTitleBar (true);
            setContentOwned (new MainComponent(), true);
            setResizable (true, true);
            setResizeLimits (760, 640, 4000, 3000);
            centreWithSize (getWidth(), getHeight());
            setVisible (true);
        }

        void closeButtonPressed() override { JUCEApplication::getInstance()->systemRequestedQuit(); }
        void lookAndFeelChanged() override
        {
            DocumentWindow::lookAndFeelChanged();
            setBackgroundColour (ui::colours().background);
        }
    };

private:
    std::unique_ptr<ui::Theme> theme; // the default look; outlives the window
    std::unique_ptr<MainWindow> mainWindow;
};

START_JUCE_APPLICATION (SecantApplication)
