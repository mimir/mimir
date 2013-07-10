from django.http import HttpResponse

def index(request):
    return HttpResponse("sup")

def read(request, lesson_id):
    return HttpResponse("You are looking at lesson %s." % lesson_id)

def question(request, lesson_id):
    return HttpResponse("You are looking at a question for lesson %s." % lesson_id)

