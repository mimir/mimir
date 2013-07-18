from django.db import models
from django.contrib.auth.models import User

class UserProfile(models.Model):
    user = models.OneToOneField(User)
    #TODO implement picture stuff properly
    gravatar_hash = models.CharField(max_length = 32)
    screen_name = models.CharField(max_length = 50)
    email = models.EmailField(max_length = 254)
    website = models.URLField()
    about = models.TextField()
    created = models.DateField(auto_now_add = True) #Creation date set on adding
    logged_in = models.DateField(auto_now_add = True) # TODO make update on all logins
