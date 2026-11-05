#include "ui/controllers/LearnWizard.h"

#include "control/virtual/VirtualPush.h"
#include "ui/theme/LatticeLookAndFeel.h"

namespace lattice::ui
{

using Step = SurfaceLearner::Step;

namespace
{
/** A role as a person names it: Virtual Push's label, without a numbered role's number. */
juce::String roleLabel(control::Role role)
{
    switch (role)
    {
    case control::Role::scene: return "Scene launch (each in turn, from the top)";
    case control::Role::trackSelect: return "Track select (each in turn, from the left)";
    case control::Role::displayUpper: return "Above the display (each in turn)";
    case control::Role::displayLower: return "Below the display (each in turn)";
    default: return control::VirtualPush::labelFor({role, 0});
    }
}

juce::String count(int n, const juce::String& one, const juce::String& many)
{
    return juce::String(n) + " " + (n == 1 ? one : many);
}
} // namespace

LearnWizard::LearnWizard(SurfaceManager& m, std::function<void()> finished) : manager(m), onFinished(std::move(finished))
{
    setTitle("Learn a controller");
    name.setTitle("Name");
    name.setText("My Controller", juce::dontSendNotification);
    inputs.setTitle("Input");
    outputs.setTitle("Output");
    for (const auto& n : manager.inputNames())
        inputs.addItem(n, inputs.getNumItems() + 1);
    outputs.addItem("None (no lights)", 1);
    for (const auto& n : manager.outputNames())
        outputs.addItem(n, outputs.getNumItems() + 1);
    inputs.onChange = [this] {
        // The output named the same, if there is one.
        for (int i = 0; i < outputs.getNumItems(); ++i)
            if (outputs.getItemText(i) == inputs.getText())
                outputs.setSelectedItemIndex(i);
    };
    inputs.setSelectedItemIndex(0, juce::sendNotificationSync);
    if (outputs.getSelectedId() == 0)
        outputs.setSelectedId(1);
    rowsBox.setTitle("Grid rows");
    columnsBox.setTitle("Grid columns");
    rowsBox.addItem("No grid", 1);
    for (int n = 1; n <= 16; ++n)
    {
        rowsBox.addItem(juce::String(n) + " rows", n + 1);
        columnsBox.addItem(juce::String(n) + " columns", n + 1);
    }
    rowsBox.setSelectedId(9);
    columnsBox.setSelectedId(9);
    start.onClick = [this] {
        const int rows = rowsBox.getSelectedId() - 1;
        begin(name.getText().trim(), inputs.getText(), outputs.getSelectedId() > 1 ? outputs.getText() : juce::String(), rows,
              rows > 0 ? columnsBox.getSelectedId() - 1 : 0);
    };

    clockwise.onClick = [this] {
        learn.encoderTurnedClockwise();
        refresh();
    };
    encoderDone.onClick = [this] {
        learn.encoderDone();
        refresh();
    };
    noMoreEncoders.onClick = [this] {
        learn.encodersFinished();
        refresh();
    };
    role.setTitle("Role");
    for (int r = 0; r < static_cast<int>(control::Role::count); ++r)
        role.addItem(roleLabel(static_cast<control::Role>(r)), r + 1);
    role.setSelectedId(1);
    assign.onClick = [this] {
        const auto r = static_cast<control::Role>(role.getSelectedId() - 1);
        if (!learn.assign({r, control::isNumbered(r) ? nextIndex(r) : 0}))
            message = juce::String("That role is taken.");
        refresh();
    };
    noMoreButtons.onClick = [this] {
        learn.buttonsFinished();
        refresh();
    };
    lightChoice.setTitle("Lights");
    for (const auto& c : SurfaceLearner::lightChoices())
        lightChoice.addItem(c.name, lightChoice.getNumItems() + 1);
    lightChoice.setSelectedId(1);
    tryIt.onClick = [this] { tryLights(lightChoice.getSelectedId() - 1); };
    useLights.onClick = [this] {
        learn.chooseLights(lightChoice.getSelectedId() - 1);
        refresh();
    };
    useNow.setToggleState(true, juce::dontSendNotification);
    saveButton.onClick = [this] {
        if (save(useNow.getToggleState()) != juce::File() && onFinished)
            onFinished();
        else
            refresh();
    };
    cancel.onClick = [this] {
        if (onFinished)
            onFinished();
    };
    for (auto* c : std::initializer_list<juce::Component*>{&name, &inputs, &outputs, &rowsBox, &columnsBox, &start, &clockwise, &encoderDone,
                                                           &noMoreEncoders, &role, &assign, &noMoreButtons, &lightChoice, &tryIt,
                                                           &useLights, &useNow, &saveButton, &cancel})
        addChildComponent(*c);
    cancel.setVisible(true);
    setSize(640, 360);
    refresh();
}

LearnWizard::~LearnWizard()
{
    manager.endLearning();
}

bool LearnWizard::begin(const juce::String& n, const juce::String& in, const juce::String& out, int rows, int columns)
{
    if (n.isEmpty())
    {
        message = "Give it a name first.";
        refresh();
        return false;
    }
    if (!manager.beginLearning(in, [this](const juce::MidiMessage& m) { heard(m); }))
    {
        message = in.isEmpty() ? juce::String("Choose its input first.") : "Can't listen to " + in + ": a surface uses it, or it's gone.";
        refresh();
        return false;
    }
    input = in;
    output = out;
    lights = out.isNotEmpty() ? manager.openOutput(out) : nullptr;
    learn.start(n, in, out, rows, columns);
    started = true;
    message.clear();
    refresh();
    return true;
}

void LearnWizard::heard(const juce::MidiMessage& m)
{
    learn.heard(m);
    message.clear();
    refresh();
}

void LearnWizard::tryLights(int choice)
{
    if (lights == nullptr)
    {
        message = "There's no output to light: it was saved without one.";
        refresh();
        return;
    }
    for (const auto& m : learn.testPattern(choice))
        lights->send(m);
    message = "Do the corners show red, green, blue and white (top-left, top-right, bottom-left, bottom-right)?";
    refresh();
}

int LearnWizard::nextIndex(control::Role r) const
{
    const auto d = learn.definition();
    return static_cast<int>(std::count_if(d.buttons.begin(), d.buttons.end(), [&](const auto& b) { return b.id.role == r; }));
}

juce::File LearnWizard::save(bool add)
{
    const auto d = learn.definition();
    auto folder = manager.userFolder();
    if (!folder.createDirectory())
    {
        message = "Couldn't make the folder " + folder.getFullPathName() + ".";
        return {};
    }
    const auto file = folder.getNonexistentChildFile(juce::File::createLegalFileName(d.name), ".surface.json", false);
    if (!file.replaceWithText(d.toJson()))
    {
        message = "Couldn't write " + file.getFullPathName() + ".";
        return {};
    }
    manager.endLearning();
    manager.reloadDefinitions();
    if (add && manager.definition(d.name) != nullptr)
        manager.add({d.name, input, output, d.grid.pads.empty()});
    return file;
}

void LearnWizard::refresh()
{
    const auto s = learn.step();
    shownStep = s;
    const bool setup = !started;
    for (auto* c : std::initializer_list<juce::Component*>{&name, &inputs, &outputs, &rowsBox, &columnsBox, &start})
        c->setVisible(setup);
    for (auto* c : std::initializer_list<juce::Component*>{&clockwise, &encoderDone, &noMoreEncoders})
        c->setVisible(started && s == Step::encoders);
    for (auto* c : std::initializer_list<juce::Component*>{&role, &assign, &noMoreButtons})
        c->setVisible(started && s == Step::buttons);
    assign.setEnabled(learn.hasPendingButton());
    for (auto* c : std::initializer_list<juce::Component*>{&lightChoice, &tryIt, &useLights})
        c->setVisible(started && s == Step::lights);
    for (auto* c : std::initializer_list<juce::Component*>{&useNow, &saveButton})
        c->setVisible(started && s == Step::done);
    resized();
    repaint();
}

void LearnWizard::paint(juce::Graphics& g)
{
    if (started && learn.step() != shownStep)
        juce::MessageManager::callAsync([safe = juce::Component::SafePointer<LearnWizard>(this)] {
            if (safe != nullptr)
                safe->refresh(); // the learner moved on without a word from here
        });
    const auto& t = themeOf(*this);
    g.fillAll(t.surface);
    auto r = textArea;
    g.setColour(t.text);
    g.setFont(juce::Font(t.font(t.fontLarge)).boldened());
    g.drawText(started ? learn.definition().name : juce::String("Learn a controller"), r.removeFromTop(juce::roundToInt(t.fontLarge * 1.6f)),
               juce::Justification::centredLeft, true);
    g.setFont(t.font(t.fontNormal));
    juce::String text = started ? learn.instruction()
                                : "Choose the controller's ports and its grid's size (rows and columns of pads), then Start. You'll press "
                                  "its pads, turn its knobs and press its buttons; Lattice works out the rest.";
    g.drawFittedText(text, r.removeFromTop(juce::roundToInt(t.fontNormal * 3.2f)), juce::Justification::topLeft, 3);
    g.setColour(t.textDim);
    juce::String status;
    if (started)
    {
        status = "Heard: " + (learn.lastHeard().isNotEmpty() ? learn.lastHeard() : juce::String("nothing yet"));
        const auto d = learn.definition();
        status << "\nLearned: " << count(static_cast<int>(d.grid.pads.size()), "pad", "pads") << ", "
               << count(learn.encodersLearned(), "encoder", "encoders") << ", " << count(learn.buttonsLearned(), "button", "buttons") << ".";
    }
    if (message.isNotEmpty())
        status << (status.isNotEmpty() ? "\n" : "") << message;
    g.drawFittedText(status, r, juce::Justification::topLeft, 4);
}

void LearnWizard::resized()
{
    const auto& t = themeOf(*this);
    const int h = juce::roundToInt(t.controlHeight);
    const int gap = juce::roundToInt(t.spacing);
    auto area = getLocalBounds().reduced(juce::roundToInt(t.padding * 2.0f));
    auto bottom = area.removeFromBottom(h);
    cancel.setBounds(bottom.removeFromRight(h * 5));
    area.removeFromBottom(gap * 2);
    // The step's controls in rows above the buttons.
    auto controls = area.removeFromBottom(h * 3 + gap * 2);
    textArea = area;
    const auto row = [&](std::initializer_list<std::pair<juce::Component*, int>> items) {
        auto r = controls.removeFromTop(h);
        controls.removeFromTop(gap);
        for (const auto& [c, width] : items)
        {
            c->setBounds(r.removeFromLeft(h * width));
            r.removeFromLeft(gap);
        }
    };
    if (!started)
    {
        row({{&name, 10}, {&rowsBox, 5}, {&columnsBox, 5}});
        row({{&inputs, 10}, {&outputs, 10}});
        row({{&start, 5}});
        return;
    }
    switch (learn.step())
    {
    case Step::encoders: row({{&clockwise, 7}, {&encoderDone, 7}, {&noMoreEncoders, 7}}); break;
    case Step::buttons: row({{&role, 8}, {&assign, 7}, {&noMoreButtons, 7}}); break;
    case Step::lights: row({{&lightChoice, 14}, {&tryIt, 3}, {&useLights, 6}}); break;
    case Step::done: row({{&useNow, 10}, {&saveButton, 5}}); break;
    case Step::grid: break;
    }
}

} // namespace lattice::ui
