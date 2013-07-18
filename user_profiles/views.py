from django.shortcuts import render
from django.http import HttpResponse, Http404

from user_profiles.models import UserProfile
from django.contrib.auth.models import User

def index(request):
    user_list = UserProfile.objects.all()
    context = ({
        'user_list':user_list,
    })
    return render(request, 'user_profiles/index.html', context)

def view(request, user_id):
    try:
        user = UserProfile.objects.get(user__id = user_id)
        context = ({'user': user,})
    except UserProfile.DoesNotExist:
        raise Http404
    return render(request, 'user_profiles/view.html', context)
