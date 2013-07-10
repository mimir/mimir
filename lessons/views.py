from django.http import HttpResponse
from django.template import RequestContext, loader

from lessons.models import Lesson

def index(request):
    latest_lesson_list = Lesson.objects.all()[:5]
    template = loader.get_template('lessons/index.html')
    context = RequestContext(request, {
        'latest_lesson_list':latest_lesson_list,
    })
    return HttpResponse(template.render(context))

def read(request, lesson_id):
    return HttpResponse("You are looking at lesson %s." % lesson_id)

def question(request, lesson_id):
    return HttpResponse("You are looking at a question for lesson %s." % lesson_id)

