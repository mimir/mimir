from django.forms import ModelForm
from community.models import UserQuestion

class QuestionForm(ModelForm):
    class Meta:
        model = UserQuestion
        fields = ['lesson','question','question_seed','title','user_question']
        labels = {
            'user_question': 'Your question',
        }
