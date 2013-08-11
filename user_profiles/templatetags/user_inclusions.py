from django.contrib.auth.models import User
from user_profiles.models import UserProfile
from django import template

register = template.Library()

@register.inclusion_tag('user_profiles/usertag.html')
def usertag(user):
    profile = UserProfile.objects.get(user__id = user.pk)
    context = ({ 'profile': profile, })
    return context
