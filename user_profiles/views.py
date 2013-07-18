from django.shortcuts import render
from django.http import HttpResponse, Http404

from user_profiles.models import UserProfile

def view(request, user_id):
    try:
        user = UserProfile.objects.get(user__id = user_id)
        context = ({'user': user,})
    except UserProfile.DoesNotExist:
        raise Http404
    return render(request, 'user_profiles/view.html', context)
