from django.shortcuts import render
from django.http import HttpResponse, Http404, HttpResponseRedirect
from django.contrib.auth import authenticate, login

from user_profiles.models import UserProfile
from user_profiles.forms import UserCreationForm
from django.contrib.auth.models import User

def index(request):
    user_list = User.objects.all()
    context = ({
        'user_list':user_list,
    })
    return render(request, 'user_profiles/index.html', context)

def view(request, user_id):
    try:
        cur_user = User.objects.get(id = user_id)
        cur_user_p = UserProfile.objects.get(user__id = user_id)
        context = ({'cur_user': cur_user, 'cur_user_p': cur_user_p,})
    except UserProfile.DoesNotExist:
        raise Http404
    return render(request, 'user_profiles/view.html', context)

def register(request):
    if request.method == 'POST':
        form = UserCreationForm(request.POST)
        if form.is_valid():
            new_user = form.save()
            username = request.POST['username']
            password = request.POST['password1']
            user = authenticate(username=username, password=password)
            login(request, user)
            import hashlib
            hash = hashlib.md5(request.POST['email'].strip().lower()).hexdigest()
            userp = UserProfile(user_id = user.id, gravatar_hash = hash, about = request.POST['about'], website = request.POST['website'])
            userp.save()
            return HttpResponseRedirect("/home/")
    else:
        form = UserCreationForm()
    return render(request, "user_profiles/register.html", {
        'form': form,
    })
