from django import forms
from django.forms.models import fields_for_model, model_to_dict
from django.contrib.auth.models import User
from django.contrib.auth.forms import UserCreationForm
from user_profiles.models import UserProfile

class UserCreationForm(forms.ModelForm):
    """
    A form that creates a user, with no privileges, from the given username and
    password. Based on the form from django.contrib.auth
    """
    error_messages = {
        'duplicate_username': "A user with that username already exists.",
        'password_mismatch': "The two password fields didn't match.",
    }
    username = forms.RegexField(label="Username", max_length=30,
        regex=r'^[\w.@+-]+$',
        help_text="Required. 30 characters or fewer. Letters, digits and "
                      "@/./+/-/_ only.",
        error_messages={
            'invalid': "This value may contain only letters, numbers and "
                         "@/./+/-/_ characters."})
    password1 = forms.CharField(label="Password",
        widget=forms.PasswordInput)
    password2 = forms.CharField(label="Password confirmation",
        widget=forms.PasswordInput,
        help_text="Enter the same password as above, for verification.")

    about = forms.CharField(label = "About",
        widget=forms.Textarea, required = False)
    website = forms.URLField(label = "Website",
        required = False)
    '''
    def __init__(self, instance=None, *args, **kwargs):
        # Add these fields from the user object
        _fields = ('first_name', 'last_name', 'email', 'username',)
        # Retrieve initial (current) data from the user object
        _initial = {}#model_to_dict(instance.user, _fields) if instance is not None else {}
        # Pass the initial data to the base
        super(UserProfileForm, self).__init__(initial=_initial, instance=instance, *args, **kwargs)
        # Retrieve the fields from the user model and update the fields with it
        self.fields.update(fields_for_model(User, _fields))
        self.fields.keyOrder = ['username','email','first_name','last_name','website','about',] 
    '''

    class Meta:
        model = User
        fields = ("username", "first_name", "last_name", "email",)

    def clean_username(self):
        # Since User.username is unique, this check is redundant,
        # but it sets a nicer error message than the ORM. See #13147.
        username = self.cleaned_data["username"]
        try:
            User._default_manager.get(username=username)
        except User.DoesNotExist:
            return username
        raise forms.ValidationError(self.error_messages['duplicate_username'])

    def clean_password2(self):
        password1 = self.cleaned_data.get("password1")
        password2 = self.cleaned_data.get("password2")
        if password1 and password2 and password1 != password2:
            raise forms.ValidationError(
                self.error_messages['password_mismatch'])
        return password2

    def save(self, commit=True):
        user = super(UserCreationForm, self).save(commit=False)
        user.set_password(self.cleaned_data["password1"])
        if commit:
            user.save()
        return user

class UserProfileForm(forms.ModelForm):
    class Meta:
        model = UserProfile
        fields = ['website', 'caption', 'about', ]
