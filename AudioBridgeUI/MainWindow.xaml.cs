using System.ComponentModel;
using System.Windows;

namespace AudioBridgeUI;

/// <summary>
/// Code-behind for MainWindow. Minimal logic -- the DataContext is set
/// externally by App.xaml.cs and all behavior lives in the view model.
/// </summary>
public partial class MainWindow : Window
{
    public MainWindow()
    {
        InitializeComponent();
    }

    /// <summary>
    /// Intercepts the window close and hides to the system tray instead.
    /// The application exits only through the tray context menu "Exit" item.
    /// </summary>
    protected override void OnClosing(CancelEventArgs e)
    {
        e.Cancel = true;
        Hide();
    }
}
