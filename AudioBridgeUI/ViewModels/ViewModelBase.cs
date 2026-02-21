using System.ComponentModel;
using System.Runtime.CompilerServices;

namespace AudioBridgeUI.ViewModels;

/// <summary>
/// Base class for all view models, providing <see cref="INotifyPropertyChanged"/> support
/// with a concise helper method.
/// </summary>
public abstract class ViewModelBase : INotifyPropertyChanged
{
    public event PropertyChangedEventHandler? PropertyChanged;

    /// <summary>
    /// Raises PropertyChanged for the specified property name.
    /// </summary>
    protected void OnPropertyChanged([CallerMemberName] string? propertyName = null) =>
        PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(propertyName));

    /// <summary>
    /// Sets the backing field and raises PropertyChanged if the value actually changed.
    /// Returns true if the value was changed.
    /// </summary>
    protected bool SetProperty<T>(ref T field, T value, [CallerMemberName] string? propertyName = null)
    {
        if (EqualityComparer<T>.Default.Equals(field, value))
            return false;

        field = value;
        OnPropertyChanged(propertyName);
        return true;
    }
}
